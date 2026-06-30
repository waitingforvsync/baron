#include "assemble_internal.h"   // the parsing vocabulary (and assemble.h via it)

#include "opcodes.h"
#include "lexer.h"
#include "expression.h"
#include "baron.h"               // the owner type the tests assemble into
#include "richc/macros.h"


#define ASSEMBLE_MAX_PASSES 100u


// Mutual recursion: a label or a scope reopens the statement loop, and the loop reaches the
// handlers that may do so. The directive handlers are referenced by the statement table.
// All the parse functions take the same head: the baron (its scopes / overlays / source files, and
// the current overlay), then the cursor `at` (source file index plus offset), the scope index, and
// the parse flags, then scratch by value. Each fetches its source rc_str from b->source_files at the top.
static parse_result handle_org(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_skip(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_skipto(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_align(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_equb(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_label(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_open_brace(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_if(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_for(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result parse_block(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result parse_scope(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result parse_one_statement(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);


// int_argument_make is shared with opcodes.c (both declared in assemble.h); it needs no token
// table, so it sits on its own. Pure: it reads v and returns the reduction.
int_argument int_argument_make(value v, bool final_pass, uint32_t at)
{
    if (value_is_numeric(v)) {
        return (int_argument) {
            .type = int_argument_type_known,
            .value = (int64_t) v.numeric
        };
    }
    
    if (value_is_error(v) && v.error == value_error_unknown_symbol) {
        if (final_pass) {
            return (int_argument) {
                .type = int_argument_type_error,
                .error = assemble_error_undefined_symbol,
                .error_at = at
            };
        }
        return (int_argument) { .type = int_argument_type_unresolved };   // a forward reference; settles later
    }

    return (int_argument) {
        .type = int_argument_type_error,
        .error = assemble_error_operand_not_numeric,
        .error_at = at
    };
}


// ---- the statement token table and the framing it drives ----

// Which block closer a lexeme_type_closer is. '}' ends a '{ }' scope, ELIF/ELSE/ENDIF the IF chain,
// NEXT a FOR body. parse_block stops at any of them; the handler that owns the block (parse_scope,
// handle_if, handle_for) dispatches on the id, and reports closer.unexpected for one it did not want.
typedef enum closer_kind {
    closer_brace,
    closer_elif,
    closer_else,
    closer_endif,
    closer_next,
} closer_kind;

// Pre-combined: every mnemonic (its own lexeme type, carrying the id) plus the statement
// directives (ORG, the '.' label introducer, the '{' scope opener) and the block closers. '#' and
// '=' never start a statement, so they live in the operand / assignment tables instead
// (operand_tokens is in opcodes.c, next to the operand parser).
static const token statement_token_entries[] = {
    {RC_STR("adc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_adc}}},
    {RC_STR("and"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_and}}},
    {RC_STR("asl"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_asl}}},
    {RC_STR("bcc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bcc}}},
    {RC_STR("bcs"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bcs}}},
    {RC_STR("beq"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_beq}}},
    {RC_STR("bit"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bit}}},
    {RC_STR("bmi"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bmi}}},
    {RC_STR("bne"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bne}}},
    {RC_STR("bpl"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bpl}}},
    {RC_STR("brk"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_brk}}},
    {RC_STR("bvc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bvc}}},
    {RC_STR("bvs"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bvs}}},
    {RC_STR("clc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_clc}}},
    {RC_STR("cld"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cld}}},
    {RC_STR("cli"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cli}}},
    {RC_STR("clv"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_clv}}},
    {RC_STR("cmp"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cmp}}},
    {RC_STR("cpx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cpx}}},
    {RC_STR("cpy"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cpy}}},
    {RC_STR("dec"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dec}}},
    {RC_STR("dex"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dex}}},
    {RC_STR("dey"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dey}}},
    {RC_STR("eor"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_eor}}},
    {RC_STR("inc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_inc}}},
    {RC_STR("inx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_inx}}},
    {RC_STR("iny"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_iny}}},
    {RC_STR("jmp"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_jmp}}},
    {RC_STR("jsr"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_jsr}}},
    {RC_STR("lda"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_lda}}},
    {RC_STR("ldx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ldx}}},
    {RC_STR("ldy"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ldy}}},
    {RC_STR("lsr"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_lsr}}},
    {RC_STR("nop"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_nop}}},
    {RC_STR("ora"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ora}}},
    {RC_STR("pha"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_pha}}},
    {RC_STR("php"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_php}}},
    {RC_STR("pla"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_pla}}},
    {RC_STR("plp"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_plp}}},
    {RC_STR("rol"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_rol}}},
    {RC_STR("ror"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ror}}},
    {RC_STR("rti"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_rti}}},
    {RC_STR("rts"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_rts}}},
    {RC_STR("sbc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sbc}}},
    {RC_STR("sec"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sec}}},
    {RC_STR("sed"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sed}}},
    {RC_STR("sei"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sei}}},
    {RC_STR("sta"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sta}}},
    {RC_STR("stx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_stx}}},
    {RC_STR("sty"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sty}}},
    {RC_STR("tax"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tax}}},
    {RC_STR("tay"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tay}}},
    {RC_STR("tsx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tsx}}},
    {RC_STR("txa"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_txa}}},
    {RC_STR("txs"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_txs}}},
    {RC_STR("tya"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tya}}},
    {RC_STR("bra"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bra}}},
    {RC_STR("dea"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dea}}},
    {RC_STR("ina"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ina}}},
    {RC_STR("phx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_phx}}},
    {RC_STR("phy"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_phy}}},
    {RC_STR("plx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_plx}}},
    {RC_STR("ply"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ply}}},
    {RC_STR("stz"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_stz}}},
    {RC_STR("clr"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_stz}}},
    {RC_STR("trb"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_trb}}},
    {RC_STR("tsb"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tsb}}},

    {RC_STR("."),      {.type = lexeme_type_keyword, .keyword = {.handle = handle_label}}},
    {RC_STR("{"),      {.type = lexeme_type_keyword, .keyword = {.handle = handle_open_brace}}},
    {RC_STR("org"),    {.type = lexeme_type_keyword, .keyword = {.handle = handle_org}}},
    {RC_STR("skip"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_skip}}},
    {RC_STR("skipto"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_skipto}}},
    {RC_STR("align"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_align}}},
    {RC_STR("equb"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equb}}},
    {RC_STR("equs"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equb}}},   // EQUS is an alias of EQUB
    {RC_STR("if"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_if}}},
    {RC_STR("for"),    {.type = lexeme_type_keyword, .keyword = {.handle = handle_for}}},
    {RC_STR("elif"),   {.type = lexeme_type_closer, .closer = {closer_elif,  assemble_error_unexpected_elif}}},
    {RC_STR("else"),   {.type = lexeme_type_closer, .closer = {closer_else,  assemble_error_unexpected_else}}},
    {RC_STR("endif"),  {.type = lexeme_type_closer, .closer = {closer_endif, assemble_error_unexpected_endif}}},
    {RC_STR("next"),   {.type = lexeme_type_closer, .closer = {closer_next,  assemble_error_unexpected_next}}},
    {RC_STR("}"),      {.type = lexeme_type_closer, .closer = {closer_brace, assemble_error_unexpected_close_brace}}},
};
static const token_table statement_tokens = RC_VIEW(statement_token_entries);

// require_separator is shared with opcodes.c (declared in assemble.h); it reads the statement
// table to recognise the '}' that implicitly closes a one-liner, so it lives here with it.
parse_result require_separator(rc_str source, uint32_t pos)
{
    lexer_result r = lexer_next(source, pos, statement_tokens);

    if (r.token.type == lexeme_type_terminator) {
        return (parse_result) { .next = r.next };
    }

    if (r.token.type == lexeme_type_closer && r.token.closer.id == closer_brace) {
        return (parse_result) { .next = pos };   // '}' closes the statement; parse_block consumes it
    }

    return parse_fail(assemble_error_expected_separator, pos);
}

static bool is_dotted(rc_str name)
{
    return rc_str_find_first(name, RC_STR(".")) != RC_INDEX_NONE;
}

// '{' is a keyword token, so it is recognised by the handler it carries.
static bool is_open_brace(lexeme lx)
{
    return lx.type == lexeme_type_keyword && lx.keyword.handle == handle_open_brace;
}

// parse_block stops at any block closer and leaves it for its caller (a scope, the file, handle_if, or
// handle_for) to read.
static bool is_block_terminator(lexeme lx)
{
    return lx.type == lexeme_type_closer;
}

// The same source cursor with its offset moved to `pos` - for the common "recurse a little further
// along the same source" call.
static cursor cursor_at(cursor at, uint32_t pos)
{
    at.pos = pos;
    return at;
}

// Fold a sub-parse's outcome into r: take its pos, OR its flags, adopt its (first) error.
static parse_result fold(parse_result r, parse_result sub)
{
    r.next        = sub.next;
    r.unresolved |= sub.unresolved;
    r.changed    |= sub.changed;

    if (sub.error != assemble_error_none) {
        r.error    = sub.error;
        r.error_at = sub.error_at;
    }
    return r;
}


// ---- directive statements ----

static parse_result handle_org(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = expression_parse(src, at.pos, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);
    }

    // A dead branch parses the operand but applies nothing - all the effect lives in this block.
    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_error:
                return parse_fail(arg.error, arg.error_at);
            case int_argument_type_known:
                overlays_org(&b->overlays, b->current_overlay, (uint32_t) (arg.value & 0xFFFF));
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown ORG leaves pc as-is this pass; forces another pass
                break;
        }
    }

    parse_result r = require_separator(src, e.next);
    r.unresolved = unresolved;
    return r;
}

// SKIP n - pad the object code with n zero bytes (advancing pc by n). A negative count would
// rewind the pointer, which we cannot do; the layout-dependent check is deferred to the final pass.
static parse_result handle_skip(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = expression_parse(src, at.pos, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_error:
                return parse_fail(arg.error, arg.error_at);
            case int_argument_type_known:
                if (arg.value < 0) {
                    if (flags.final) return parse_fail(assemble_error_skip_backwards, at.pos);
                }
                else {
                    overlays_skip(&b->overlays, b->current_overlay, (uint32_t) arg.value);
                }
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown count emits nothing; forces another pass
                break;
        }
    }

    parse_result r = require_separator(src, e.next);
    r.unresolved = unresolved;
    return r;
}

// SKIPTO addr - pad with zeroes until pc reaches addr. Being already past addr is an error,
// deferred to the final pass since pc only settles once preceding forward references resolve.
static parse_result handle_skipto(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = expression_parse(src, at.pos, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_error:
                return parse_fail(arg.error, arg.error_at);
            case int_argument_type_known: {
                uint32_t pc = overlays_pc(&b->overlays, b->current_overlay);
                if (arg.value < pc) {
                    if (flags.final) return parse_fail(assemble_error_skip_backwards, at.pos);
                }
                else {
                    overlays_skip(&b->overlays, b->current_overlay, (uint32_t) (arg.value - pc));
                }
                break;
            }
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown target emits nothing; forces another pass
                break;
        }
    }

    parse_result r = require_separator(src, e.next);
    r.unresolved = unresolved;
    return r;
}

// ALIGN n - pad with zeroes until pc is a multiple of n. n < 1 is meaningless (and would divide
// by zero), so it is an error; the modulo is only evaluated once we know n is sound.
static parse_result handle_align(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = expression_parse(src, at.pos, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_error:
                return parse_fail(arg.error, arg.error_at);
            case int_argument_type_known:
                if (arg.value < 1) {
                    if (flags.final) return parse_fail(assemble_error_bad_alignment, at.pos);
                }
                else {
                    uint32_t n = (uint32_t) arg.value;
                    uint32_t rem = overlays_pc(&b->overlays, b->current_overlay) % n;
                    if (rem != 0) {
                        overlays_skip(&b->overlays, b->current_overlay, n - rem);
                    }
                }
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown alignment emits nothing; forces another pass
                break;
        }
    }

    parse_result r = require_separator(src, e.next);
    r.unresolved = unresolved;
    return r;
}

// Emit one value's bytes into the current overlay, for EQUB / EQUS. A string goes character by
// character; a range is enumerated; a list is descended (so nested lists and ranges flatten out);
// anything else is a single byte via int_argument_make, where a forward reference emits a 0 placeholder
// and asks for another pass. Returns .error/.error_at on failure and .unresolved when a value defers
// (its .next is unused). A dead branch emits nothing and raises nothing - mirroring inactive statements.
static parse_result emit_data(baron *b, value v, parse_flags flags, uint32_t at, rc_arena scratch)
{
    if (!flags.active) {
        return (parse_result) {0};
    }

    if (value_is_string(v)) {
        for (uint32_t i = 0; i < v.string.len; i++) {
            overlays_emit_u8(&b->overlays, b->current_overlay, (uint8_t) v.string.data[i]);
        }
        return (parse_result) {0};
    }

    if (value_is_range(v)) {
        return emit_data(b, range_to_list(v.range, &scratch), flags, at, scratch);   // enumerated to a list (or an error leaf)
    }

    if (value_is_list(v)) {
        parse_result acc = {0};
        for (uint32_t i = 0; i < v.list.num; i++) {
            parse_result em = emit_data(b, rc_view_value_get(v.list, i), flags, at, scratch);
            if (em.error != assemble_error_none) {
                return em;
            }
            acc.unresolved |= em.unresolved;
        }
        return acc;
    }

    // A numeric, a forward reference, or some other error value - one byte either way.
    int_argument arg = int_argument_make(v, flags.final, at);
    if (arg.type == int_argument_type_error) {
        return parse_fail(arg.error, arg.error_at);
    }
    if (arg.type == int_argument_type_unresolved) {
        overlays_emit_u8(&b->overlays, b->current_overlay, 0);   // placeholder; its size is assumed one byte
        return (parse_result) {.unresolved = true};
    }
    // We allow signed bytes, so the byte-sized window is -255..255; checked only once everything settles.
    if (flags.final && (arg.value < -255 || arg.value > 255)) {
        return parse_fail(assemble_error_value_out_of_range, at);
    }
    overlays_emit_u8(&b->overlays, b->current_overlay, (uint8_t) (arg.value & 0xFF));
    return (parse_result) {0};
}

// EQUB / EQUS: a comma-separated list of values, each emitted as bytes (see emit_data). The two
// spellings are aliases - both take numbers, strings, ranges and lists alike.
static parse_result handle_equb(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;

    while (true) {
        expr_result e = expression_parse(src, pos, &b->scopes, scope, &scratch);
        if (e.error != expr_error_none) {
            return parse_fail(assemble_error_expression, e.error_at);
        }

        parse_result em = emit_data(b, e.value, flags, pos, scratch);
        if (em.error != assemble_error_none) {
            return em;
        }
        unresolved |= em.unresolved;

        lexer_result lr = lexer_next(src, e.next, statement_tokens);
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        parse_result r = require_separator(src, e.next);
        r.unresolved = unresolved;
        return r;   // end of the list: a terminator or '}'
    }
}

static parse_result handle_label(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The label name. A name spelled exactly like a mnemonic would lex as that opcode (the same
    // limitation an assignment target has at statement start) - acceptable, and not worth a
    // private name table.
    lexer_result nm = lexer_next(src, at.pos, statement_tokens);
    if (nm.token.type != lexeme_type_identifier || is_dotted(nm.token.identifier.name)) {
        return parse_fail(assemble_error_expected_label_name, at.pos);
    }
    rc_str name = nm.token.identifier.name;

    // Bind name = current pc in the current scope; a moved label drives another pass, and a second
    // definition of the same name here (a different source position) is a duplicate. In a dead branch
    // we instead REMOVE the name, clearing any binding a live earlier pass left behind.
    parse_result r = {.next = nm.next};
    if (flags.active) {
        symbol_status st = scopes_set_symbol(
            &b->scopes,
            scope,
            name,
            value_make_numeric((double)overlays_pc(&b->overlays, b->current_overlay)),
            at
        );

        if (st == symbol_status_duplicate) {
            return parse_fail(assemble_error_duplicate_symbol, at.pos);
        }
        r.changed = (st == symbol_status_changed);
    }
    else {
        r.changed = scopes_remove_symbol(&b->scopes, scope, name);
    }

    // What follows decides the label's shape. A '{' - optionally one separator away, so it may sit
    // on the next line - makes the label name a scope. Anything else is not the label's to parse: we
    // drop back to the parent loop, which takes the next token as its own statement (or, on a '}',
    // closes the block). The label never owns the statement that follows it.
    lexer_result nb = lexer_next(src, r.next, statement_tokens);
    
    if (nb.token.type == lexeme_type_terminator) {
        lexer_result after = lexer_next(src, nb.next, statement_tokens);
        if (!is_open_brace(after.token)) {
            return r;                          // stand-alone label - leave the terminator for the loop
        }
        nb = after;                            // .label <separator> { ... } still names the scope
    }

    if (is_open_brace(nb.token)) {
        uint32_t child = scopes_get_or_make_child(&b->scopes, scope, name);
        return fold(r, parse_scope(b, cursor_at(at, nb.next), child, flags, scratch));
    }

    return r;   // not a scope - leave the rest of the line to the parent parser
}

// Build a scope key no user identifier can spell - '@' source ':' pos - into the caller's fixed buffer.
// '@' and ':' are not identifier characters, so the key never collides with a real name. FOR appends a
// further ':' iteration; the brace handler uses it as-is. Each u32 is at most ten digits, so the buffer
// never needs to grow (hence the NULL arena); scopes_get_or_make_child owns a copy on first sighting.
static rc_mstr anon_scope_key(char *storage, uint32_t cap, cursor at)
{
    rc_mstr m = {.data = storage, .len = 0, .cap = cap};
    rc_mstr_append_char(&m, '@', NULL);
    rc_mstr_append_u32(&m, at.source, NULL);
    rc_mstr_append_char(&m, ':', NULL);
    rc_mstr_append_u32(&m, at.pos, NULL);
    return m;
}

// The key for one FOR iteration: the FOR's site key (anon_scope_key) plus ':' iteration, so iteration i
// has a stable identity across passes (and body labels are private to it, never colliding across iters).
static rc_mstr anon_for_scope_key(char *storage, uint32_t cap, cursor at, uint32_t iteration)
{
    rc_mstr m = anon_scope_key(storage, cap, at);
    rc_mstr_append_char(&m, ':', NULL);
    rc_mstr_append_u32(&m, iteration, NULL);
    return m;
}

static parse_result handle_open_brace(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    // The just-passed pos gives the anonymous scope a stable per-pass identity, so re-walking it
    // on a later pass keeps the same bindings.
    char storage[64];
    rc_mstr key = anon_scope_key(storage, sizeof storage, at);
    uint32_t child = scopes_get_or_make_child(&b->scopes, scope, key.view);
    return parse_scope(b, at, child, flags, scratch);
}

// IF cond ... ELIF cond ... ELSE ... ENDIF, evaluated each pass. IF does not open a scope: labels in
// the live branch leak to the enclosing scope, and the construct must close (ENDIF) inside the same
// scope it opened in. ELIF is just an IF in the else position (IF a x ELIF b y ELSE z == IF a x ELSE
// {IF b y ELSE z}), so this reads that way: one condition picks between a true body, active iff
// flags.active && the condition holds, and an else body, active iff flags.active && the condition is
// known FALSE. At most one is live - and neither when the condition cannot yet be evaluated, which
// owes another pass (a hard error on the final pass). Entered just past the IF - or, via the
// recursion, the ELIF - at the condition.
static parse_result handle_if(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = expression_parse(src, at.pos, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);   // a syntax error always aborts
    }

    parse_result sep = require_separator(src, e.next);
    if (sep.error != assemble_error_none) {
        return sep;
    }

    // Judge the condition only when the parent is live. if_cond and else_cond are mutually exclusive
    // (and both false when the condition is unevaluable - then we owe another pass).
    bool if_cond = false;
    bool else_cond = false;
    parse_result acc = {.next = sep.next};

    if (flags.active) {
        int_argument cond = int_argument_make(e.value, flags.final, at.pos);
        if (cond.type == int_argument_type_error) {
            return parse_fail(cond.error, cond.error_at);
        }

        if (cond.type == int_argument_type_unresolved) {
            acc.unresolved = true;
        }
        else {
            if_cond = (cond.value != 0);
            else_cond = !if_cond;
        }
    }

    // The condition true body
    acc = fold(
        acc,
        parse_block(
            b,
            cursor_at(at, acc.next),
            scope,
            (parse_flags) {flags.final, flags.active && if_cond},
            scratch
        )
    );

    if (acc.error != assemble_error_none) {
        return acc;
    }

    lexer_result t = lexer_next(src, acc.next, statement_tokens);

    // If the IF block is closed with an ELIF, recurse here
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_elif) {
        return fold(
            acc,
            handle_if(
                b,
                cursor_at(at, t.next),
                scope,
                (parse_flags) {flags.final, flags.active && else_cond},
                scratch
            )
        );
    }

    // If it was an ELSE, parse a black with the else_cond. This is not necessarily the inverse of the if_cond,
    // because the condition may be unevaluable, in which case both branches are dead.
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_else) {
        // ELSE insists on a separator after it, like the IF condition and ENDIF do.
        parse_result sep = require_separator(src, t.next);
        if (sep.error != assemble_error_none) {
            return sep;
        }

        acc = fold(
            acc,
            parse_block(
                b,
                cursor_at(at, sep.next),
                scope,
                (parse_flags) {flags.final, flags.active && else_cond},
                scratch
            )
        );

        if (acc.error != assemble_error_none) {
            return acc;
        }

        // Now read the token after the ELSE block, which must be ENDIF. The ELSE body is always the last
        t = lexer_next(src, acc.next, statement_tokens);
    }

    // If it was ENDIF, close the IF block here
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_endif) {
        acc.next = t.next;
        return fold(acc, require_separator(src, acc.next));
    }

    // A chain keyword out of place (a second ELSE, an ELIF after ELSE) is unexpected - raise its own
    // error; any other closer ('}', NEXT) or end of input means there was no ENDIF at all.
    acc.error = t.token.type == lexeme_type_closer
                    && (t.token.closer.id == closer_elif || t.token.closer.id == closer_else)
                ? (assemble_error) t.token.closer.unexpected
                : assemble_error_unclosed_if;
    acc.error_at = acc.next;
    return acc;
}


// ---- the assignment statement ----

// '=' is its own tiny table, lexed only after an identifier at statement start.
static const token assign_token_entries[] = {
    {RC_STR("="), {.type = lexeme_type_assign}},
};
static const token_table assign_tokens = RC_VIEW(assign_token_entries);

// `name` is the identifier and `pos` sits just past it; an assignment defines a symbol and
// emits nothing.
static parse_result handle_assignment(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_str name, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    if (is_dotted(name)) {
        return parse_fail(assemble_error_invalid_assignment, at.pos);
    }

    lexer_result eq = lexer_next(src, at.pos, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return parse_fail(assemble_error_expected_assign, at.pos);
    }

    expr_result e = expression_parse(src, eq.next, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);
    }

    // Bind name = value. A second definition of the same name here (a different source position) is a
    // duplicate. In a dead branch we instead REMOVE the name, clearing a binding a live earlier pass
    // left behind, and we raise none of the value-dependent errors below.
    parse_result r = {.next = e.next};
    if (flags.active) {
        symbol_status st = scopes_set_symbol(&b->scopes, scope, name, e.value, at);

        if (st == symbol_status_duplicate) {
            return parse_fail(assemble_error_duplicate_symbol, at.pos);
        }

        if (value_is_error(e.value) && e.value.error == value_error_unknown_symbol) {
            if (flags.final) {
                return parse_fail(assemble_error_undefined_symbol, eq.next);
            }
            r.unresolved = true;   // a forward reference in the value; settles on a later pass
        }

        r.changed = (st == symbol_status_changed);
    }
    else {
        r.changed = scopes_remove_symbol(&b->scopes, scope, name);
    }
    
    return fold(r, require_separator(src, e.next));
}


// ---- the FOR loop ----

// FOR <var> = <range-or-list> : ... : NEXT, evaluated each pass. Each iteration runs the body once with
// <var> bound to that element in its own per-iteration child scope, so body labels never collide across
// iterations. An empty sequence (e.g. {} or 5..<5) runs the body once inactive - zero bytes, no error.
// A sequence whose count is not yet known (a forward reference) does the same and forces another pass;
// on the final pass that is a hard undefined_symbol. Like IF, FOR opens no scope of its own for the loop
// control (only the per-iteration body scopes) and must close (NEXT) inside the scope it began in.
static parse_result handle_for(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The loop variable: a bare identifier (a dotted path cannot be a binding target).
    lexer_result var = lexer_next(src, at.pos, statement_tokens);
    if (var.token.type != lexeme_type_identifier || is_dotted(var.token.identifier.name)) {
        return parse_fail(assemble_error_expected_label_name, at.pos);
    }
    rc_str name = var.token.identifier.name;

    lexer_result eq = lexer_next(src, var.next, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return parse_fail(assemble_error_expected_assign, var.next);
    }

    expr_result e = expression_parse(src, eq.next, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) {
        return parse_fail(assemble_error_expression, e.error_at);
    }

    parse_result sep = require_separator(src, e.next);
    if (sep.error != assemble_error_none) {
        return sep;
    }
    uint32_t body_start = sep.next;

    // Reduce the sequence to its element list, but only when the parent is live (a dead FOR evaluates
    // nothing, like IF). iterate stays false for an empty list, an unresolved count, or a dead parent.
    bool iterate = false;
    bool unresolved = false;
    rc_view_value items = {0};
    if (flags.active) {
        value seq = e.value;
        if (value_is_error(seq) && seq.error == value_error_unknown_symbol) {
            if (flags.final) {
                return parse_fail(assemble_error_undefined_symbol, eq.next);
            }
            unresolved = true;   // the count is not known yet; defer and try again next pass
        }
        else {
            if (value_is_range(seq)) {
                seq = range_to_list(seq.range, &scratch);
            }
            if (value_is_list(seq)) {
                items = seq.list;
                iterate = (items.num > 0);   // an empty list is legal: it runs the body zero times
            }
            else {
                return parse_fail(assemble_error_not_iterable, eq.next);   // scalar/string/none, or a range error
            }
        }
    }

    // Run the body: once per element when iterating, otherwise a single inactive structural pass (which
    // is also how we locate NEXT). Every pass re-walks the same body span and returns .next at NEXT.
    uint32_t count = iterate ? items.num : 1;
    bool active_body = iterate;   // iterate implies flags.active; empty / unresolved / dead parse inactive
    parse_result acc = {.next = body_start, .unresolved = unresolved};
    for (uint32_t i = 0; i < count && acc.error == assemble_error_none; i++) {
        char storage[64];
        rc_mstr key = anon_for_scope_key(storage, sizeof storage, at, i);
        uint32_t child = scopes_get_or_make_child(&b->scopes, scope, key.view);
        if (iterate) {
            scopes_set_symbol(&b->scopes, child, name, rc_view_value_get(items, i), at);
        }
        acc = fold(acc, parse_block(b, cursor_at(at, body_start), child,
                                    (parse_flags) {flags.final, active_body}, scratch));
    }
    if (acc.error != assemble_error_none) {
        return acc;
    }

    // Close with NEXT, which insists on a separator after it (like ENDIF).
    lexer_result t = lexer_next(src, acc.next, statement_tokens);
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_next) {
        acc.next = t.next;
        return fold(acc, require_separator(src, acc.next));
    }
    acc.error = assemble_error_unclosed_for;   // a '}' / end of input / foreign keyword before NEXT
    acc.error_at = acc.next;
    return acc;
}


// ---- the statement loop ----

static parse_result parse_one_statement(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, at.pos, statement_tokens);

    switch (lr.token.type) {
        case lexeme_type_opcode:
            return opcode_parse(b, (mnemonic)lr.token.opcode.id, cursor_at(at, lr.next), scope, flags, scratch);
        case lexeme_type_keyword:
            return lr.token.keyword.handle(b, cursor_at(at, lr.next), scope, flags, scratch);
        case lexeme_type_identifier:
            return handle_assignment(b, cursor_at(at, lr.next), scope, flags, lr.token.identifier.name, scratch);
        default:
            return parse_fail(assemble_error_unexpected_token, at.pos);
    }
}

// Parse statements until the block's closer: a '}' or end of input. It stops AT the closer -
// leaving a '}' unconsumed - and treats neither as an error, because which closer is required
// depends on the caller: parse_scope wants the '}', run_pass wants end of input.
static parse_result parse_block(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    parse_result acc = {.next = at.pos};
    while (acc.error == assemble_error_none) {
        lexer_result lr = lexer_next(src, acc.next, statement_tokens);

        if (is_block_terminator(lr.token)) {
            return acc;                    // stop at the closer, leaving it for the caller
        }

        if (lr.token.type == lexeme_type_terminator) {
            if (lexer_at_end(src, lr.next)) {
                acc.next = lr.next;
                return acc;                // stop at end of input
            }
            acc.next = lr.next;            // blank statement
            continue;
        }

        acc = fold(acc, parse_one_statement(b, cursor_at(at, acc.next), scope, flags, scratch));
    }
    return acc;
}

// A braced block: parse its statements (entered just past the '{') and require the closing '}';
// reaching end of input first is an unclosed scope.
static parse_result parse_scope(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    parse_result r = parse_block(b, at, scope, flags, scratch);

    if (r.error != assemble_error_none) {
        return r;
    }

    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, r.next, statement_tokens);

    if (lr.token.type == lexeme_type_closer) {
        if (lr.token.closer.id == closer_brace) {   // the '}' we were waiting for
            r.next = lr.next;
            return r;
        }
        r.error    = (assemble_error) lr.token.closer.unexpected;   // an IF/FOR closer with nothing to close
        r.error_at = lr.next;
        return r;
    }

    r.error = assemble_error_unclosed_scope;   // end of input before the '}'
    r.error_at = r.next;
    return r;
}


// A whole source file - the largest parsable item: parse its statements and require they run right
// out at end of input. parse_block stops only at a block closer or at end of input, so a leftover
// closer here has nothing to close: a stray '}', an IF-chain keyword with no IF, or a NEXT with no
// FOR. With those ruled out, end of input is the only thing left (asserted). (parse_scope is a braced
// block within a file; parse_block is the shared statement loop both rest on.)
static parse_result parse_file(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    parse_result r = parse_block(b, at, scope, flags, scratch);

    if (r.error != assemble_error_none) {
        return r;
    }

    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, r.next, statement_tokens);

    if (lr.token.type == lexeme_type_closer) {   // any closer at file scope has nothing to close
        r.error = (assemble_error) lr.token.closer.unexpected;
        r.error_at = lr.next;
        return r;
    }

    RC_ASSERT(lexer_at_end(src, r.next));   // parse_block had no other reason to stop
    return r;
}


// ---- the multi-pass driver ----

static parse_result run_pass(baron *b, uint32_t source, parse_flags flags, rc_arena scratch)
{
    overlays_reset_all(&b->overlays);
    b->current_overlay = overlays_default;   // each pass re-derives the current overlay from the source

    const uint32_t scope = 0;
    return parse_file(
        b,
        (cursor) {.source = source, .pos = 0},
        scope,
        flags,
        scratch
    );
}

// The shared core: run passes over the source already cached at index `source` in b, until the
// labels and forward references settle (or non-convergence). assemble_string / assemble_file are
// thin wrappers that establish the source entry, then call this.
static assemble_result run_passes(baron *b, uint32_t source, rc_arena scratch)
{
    for (uint32_t pass = 1; pass <= ASSEMBLE_MAX_PASSES; pass++) {
        parse_result r = run_pass(
            b,
            source,
            (parse_flags) {.active = true},
            scratch
        );

        if (r.error != assemble_error_none) {
            return (assemble_result) {
                .passes = pass,
                .error = r.error,
                .error_at = r.error_at
            };
        }

        if (!r.unresolved && !r.changed) {
            // Settled. One more pass with checks armed, to surface any deferred range errors.
            parse_result fin = run_pass(
                b,
                source,
                (parse_flags) {.final = true, .active = true},
                scratch
            );
            
            if (fin.error != assemble_error_none) {
                return (assemble_result) {
                    .passes = pass + 1,
                    .error = fin.error,
                    .error_at = fin.error_at
                };
            }

            return (assemble_result) {
                .code = overlays_code(&b->overlays, overlays_default),
                .passes = pass + 1
            };
        }
    }

    // Did not settle within the cap. A final diagnostic pass names a concrete cause (an undefined
    // symbol) where there is one; otherwise it is genuine oscillation.
    parse_result diag = run_pass(
        b,
        source,
        (parse_flags) {.final = true, .active = true},
        scratch
    );

    if (diag.error != assemble_error_none) {
        return (assemble_result) {
            .passes = ASSEMBLE_MAX_PASSES + 1,
            .error = diag.error,
            .error_at = diag.error_at
        };
    }

    return (assemble_result) {
        .passes = ASSEMBLE_MAX_PASSES,
        .error = assemble_error_no_convergence
    };
}

assemble_result assemble_string(baron *b, rc_str name, rc_str text, rc_arena scratch)
{
    RC_ASSERT(b != NULL);
    return run_passes(b, source_files_add_string(&b->source_files, name, text), scratch);
}

assemble_result assemble_file(baron *b, rc_str path, rc_arena scratch)
{
    RC_ASSERT(b != NULL);
    uint32_t source = source_files_add_file(&b->source_files, path);
    if (source == RC_INDEX_NONE) {
        return (assemble_result) {
            .error = assemble_error_source_load
        };
    }
    return run_passes(b, source, scratch);
}


rc_str assemble_error_name(assemble_error e)
{
    switch (e) {
        case assemble_error_none:                   return RC_STR("none");
        case assemble_error_unexpected_token:       return RC_STR("unexpected_token");
        case assemble_error_unexpected_close_brace: return RC_STR("unexpected_close_brace");
        case assemble_error_unclosed_scope:         return RC_STR("unclosed_scope");
        case assemble_error_expected_separator:     return RC_STR("expected_separator");
        case assemble_error_expected_label_name:    return RC_STR("expected_label_name");
        case assemble_error_invalid_assignment:     return RC_STR("invalid_assignment");
        case assemble_error_expected_assign:        return RC_STR("expected_assign");
        case assemble_error_expected_close_paren:   return RC_STR("expected_close_paren");
        case assemble_error_bad_index_register:     return RC_STR("bad_index_register");
        case assemble_error_missing_operand:        return RC_STR("missing_operand");
        case assemble_error_bad_addressing_mode:    return RC_STR("bad_addressing_mode");
        case assemble_error_operand_not_numeric:    return RC_STR("operand_not_numeric");
        case assemble_error_value_out_of_range:     return RC_STR("value_out_of_range");
        case assemble_error_branch_out_of_range:    return RC_STR("branch_out_of_range");
        case assemble_error_skip_backwards:         return RC_STR("skip_backwards");
        case assemble_error_bad_alignment:          return RC_STR("bad_alignment");
        case assemble_error_undefined_symbol:       return RC_STR("undefined_symbol");
        case assemble_error_duplicate_symbol:       return RC_STR("duplicate_symbol");
        case assemble_error_unclosed_if:            return RC_STR("unclosed_if");
        case assemble_error_unexpected_elif:        return RC_STR("unexpected_elif");
        case assemble_error_unexpected_else:        return RC_STR("unexpected_else");
        case assemble_error_unexpected_endif:       return RC_STR("unexpected_endif");
        case assemble_error_unclosed_for:           return RC_STR("unclosed_for");
        case assemble_error_unexpected_next:        return RC_STR("unexpected_next");
        case assemble_error_not_iterable:           return RC_STR("not_iterable");
        case assemble_error_expression:             return RC_STR("expression");
        case assemble_error_no_convergence:         return RC_STR("no_convergence");
        case assemble_error_source_load:            return RC_STR("source_load");
    }
    RC_UNREACHABLE();
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST_GROUP_DATA(assemble) {
    baron    b;
    rc_arena scratch;
};

RC_TEST_GROUP_INIT(assemble, fix)
{
    baron_init(&fix->b);
    fix->scratch = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(assemble, fix)
{
    baron_deinit(&fix->b);
    rc_arena_deinit(&fix->scratch);
}

// Name each test source after its own text, so distinct snippets get distinct source-cache
// entries (the cache is keyed by name) even though they share one baron across the step.
#define RESULT(src) assemble_string(&fix->b, RC_STR(src), RC_STR(src), fix->scratch)

// Whether a clean result holds exactly the given bytes.
static bool code_is(assemble_result r, const uint8_t *exp, uint32_t n)
{
    if (r.error != assemble_error_none || r.code.num != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (rc_view_bytes_get(r.code, i) != exp[i]) {
            return false;
        }
    }
    return true;
}

RC_TEST_STEP(assemble, addressing_modes, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("LDA #0"),       (uint8_t[]) {0xA9, 0x00}, 2));
    RC_CHECK_TRUE(code_is(RESULT("LDA &70"),      (uint8_t[]) {0xA5, 0x70}, 2));
    RC_CHECK_TRUE(code_is(RESULT("LDA &2000"),    (uint8_t[]) {0xAD, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(RESULT("LDA &70,X"),    (uint8_t[]) {0xB5, 0x70}, 2));
    RC_CHECK_TRUE(code_is(RESULT("LDA &2000,X"),  (uint8_t[]) {0xBD, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(RESULT("LDX &70,Y"),    (uint8_t[]) {0xB6, 0x70}, 2));
    RC_CHECK_TRUE(code_is(RESULT("LDA (&70),Y"),  (uint8_t[]) {0xB1, 0x70}, 2));
    RC_CHECK_TRUE(code_is(RESULT("STA (&70,X)"),  (uint8_t[]) {0x81, 0x70}, 2));
    RC_CHECK_TRUE(code_is(RESULT("JMP (&1234)"),  (uint8_t[]) {0x6C, 0x34, 0x12}, 3));
    RC_CHECK_TRUE(code_is(RESULT("JMP &70"),      (uint8_t[]) {0x4C, 0x70, 0x00}, 3));   // no zp JMP
    RC_CHECK_TRUE(code_is(RESULT("ASL"),          (uint8_t[]) {0x0A}, 1));
    RC_CHECK_TRUE(code_is(RESULT("ASL A"),        (uint8_t[]) {0x0A}, 1));
    RC_CHECK_TRUE(code_is(RESULT("NOP"),          (uint8_t[]) {0xEA}, 1));
    RC_CHECK_TRUE(code_is(RESULT("STA &70"),      (uint8_t[]) {0x85, 0x70}, 2));
}

RC_TEST_STEP(assemble, multiple_statements, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("LDA #1 : STA &70 : RTS"),
                          (uint8_t[]){0xA9, 0x01, 0x85, 0x70, 0x60}, 5));
    RC_CHECK_TRUE(code_is(RESULT("LDA #1\nSTA &70\nRTS"),
                          (uint8_t[]){0xA9, 0x01, 0x85, 0x70, 0x60}, 5));
}

RC_TEST_STEP(assemble, branch_offsets, fix)
{
    // Backward: target at pc 0, NOP, then BNE back to it. offset = 0 - (1 + 2) = -3 = 0xFD.
    RC_CHECK_TRUE(code_is(RESULT(".t NOP : BNE t"), (uint8_t[]){0xEA, 0xD0, 0xFD}, 3));
    // Forward: BEQ over a following NOP. BEQ at 0, NOP at 2, target = 3, offset = 3 - 2 = 1.
    // (`over` not `skip`: SKIP is now a reserved directive keyword, like a mnemonic.)
    RC_CHECK_TRUE(code_is(RESULT("BEQ over : NOP : .over"), (uint8_t[]){0xF0, 0x01, 0xEA}, 3));
}

RC_TEST_STEP(assemble, branch_to_shadowed_forward_label, fix)
{
    // `.label` names the scope, and an inner `.label` shadows it. On the first pass the inner label
    // is not yet bound, so BEQ transiently resolves to the FAR outer label (256+ bytes back) - a
    // branch that would be out of range. That error must be withheld on non-final passes; by the
    // time the layout settles the near inner label is bound and the branch is in range (offset 0).
    assemble_result r = RESULT(".label { SKIP 256 : BEQ label : .label }");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK(r.code.num, ==, 258u);                                  // 256 skipped + BEQ (2 bytes)
    RC_CHECK((uint32_t)rc_view_bytes_get(r.code, 256), ==, 0xF0u);   // BEQ opcode
    RC_CHECK((uint32_t)rc_view_bytes_get(r.code, 257), ==, 0x00u);   // resolves near: branch to the next byte
}

RC_TEST_STEP(assemble, skip_skipto_align, fix)
{
    // SKIP n emits n zero bytes.
    RC_CHECK_TRUE(code_is(RESULT("LDA #1 : SKIP 3 : RTS"), (uint8_t[]) {0xA9, 0x01, 0x00, 0x00, 0x00, 0x60}, 6));

    // SKIPTO addr fills with zeroes until pc reaches addr (0x2001 -> 0x2004 = three bytes).
    RC_CHECK_TRUE(code_is(RESULT("ORG &2000 : NOP : SKIPTO &2004 : RTS"), (uint8_t[]) {0xEA, 0x00, 0x00, 0x00, 0x60}, 5));
    // Already past addr is an error.
    RC_CHECK_TRUE(RESULT("ORG &2000 : NOP : NOP : SKIPTO &2001").error == assemble_error_skip_backwards);

    // ALIGN n pads up to the next multiple of n (pc 1 -> 4 = three bytes)...
    RC_CHECK_TRUE(code_is(RESULT("NOP : ALIGN 4 : RTS"), (uint8_t[]) {0xEA, 0x00, 0x00, 0x00, 0x60}, 5));
    // ...and is a no-op when pc already sits on the boundary.
    RC_CHECK_TRUE(code_is(RESULT("ALIGN 4 : NOP"), (uint8_t[]) {0xEA}, 1));
    // ALIGN 0 is meaningless.
    RC_CHECK_TRUE(RESULT("ALIGN 0").error == assemble_error_bad_alignment);
}

RC_TEST_STEP(assemble, skip_advances_pc, fix)
{
    // SKIP moves pc, so a label after it sees the advanced address.
    assemble_result r = RESULT("ORG &2000 : .a SKIP 4 : .b");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("a")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("b")), value_make_numeric(0x2004)));
}

RC_TEST_STEP(assemble, org_and_labels, fix)
{
    // ORG sets the label's value but not where code lands (code still fills from index 0).
    assemble_result r = RESULT("ORG &2000 : LDA #1 : .here");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")),
                                 value_make_numeric(0x2002)));
}

RC_TEST_STEP(assemble, symbol_definition, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("n = 5 : LDA #n"), (uint8_t[]){0xA9, 0x05}, 2));
}

RC_TEST_STEP(assemble, duplicate_symbol_is_rejected, fix)
{
    // Two definitions of one name in the same scope - whether labels or assignments - are a
    // duplicate. Distinct names per snippet, since these share one baron's persistent scopes.
    RC_CHECK_TRUE(RESULT(".dup NOP : .dup RTS").error == assemble_error_duplicate_symbol);
    RC_CHECK_TRUE(RESULT("twice = 1 : twice = 2").error == assemble_error_duplicate_symbol);
}

RC_TEST_STEP(assemble, inner_scope_mirrors_outer_name, fix)
{
    // The same name in a nested scope is fine: the inner binding lives in its own scope.
    RC_CHECK_TRUE(RESULT("x = 1 : { x = 2 }").error == assemble_error_none);
}

RC_TEST_STEP(assemble, forward_reference_is_order_independent, fix)
{
    // A forward reference to a zero-page value converges to zero-page, the same as if it had
    // been defined first.
    assemble_result fwd = RESULT("LDA foo : foo = &70");
    RC_CHECK_TRUE(code_is(fwd, (uint8_t[]){0xA5, 0x70}, 2));
    RC_CHECK_TRUE(fwd.passes >= 2);
}

RC_TEST_STEP(assemble, forward_reference_to_absolute, fix)
{
    assemble_result fwd = RESULT("LDA foo : foo = &2000");
    RC_CHECK_TRUE(code_is(fwd, (uint8_t[]){0xAD, 0x00, 0x20}, 3));
}

RC_TEST_STEP(assemble, named_scope_dotted_access, fix)
{
    assemble_result r = RESULT("ORG &2000 : .routine { .core LDA #0 } : x = routine.core");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("x")),
                                 value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("routine.core")),
                                 value_make_numeric(0x2000)));
}

RC_TEST_STEP(assemble, named_scope_brace_after_separator, fix)
{
    // The naming brace may sit on the next line (or after a ':').
    assemble_result r = RESULT("ORG &2000\n.routine\n{\n.core LDA #0\n}\nx = routine.core");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("routine.core")),
                                 value_make_numeric(0x2000)));
}

RC_TEST_STEP(assemble, anonymous_scope_is_private, fix)
{
    assemble_result r = RESULT("{ y = 5 }");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("y"))));
}

RC_TEST_STEP(assemble, label_does_not_own_following_statement, fix)
{
    // A label hands control straight back: a '}' may sit on the same line right after it (the label
    // no longer swallows what follows), and the inner label still binds to the current pc.
    assemble_result r = RESULT("ORG &2000 : .r { .e }");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("r.e")), value_make_numeric(0x2000)));
}

RC_TEST_STEP(assemble, errors, fix)
{
    RC_CHECK_TRUE(RESULT("a.b = 1").error            == assemble_error_invalid_assignment);
    RC_CHECK_TRUE(RESULT("}").error                  == assemble_error_unexpected_close_brace);
    RC_CHECK_TRUE(RESULT("LDA").error                == assemble_error_missing_operand);
    RC_CHECK_TRUE(RESULT("LDA #1 LDA #2").error      == assemble_error_expected_separator);
    RC_CHECK_TRUE(RESULT("LDA missing").error        == assemble_error_undefined_symbol);
    RC_CHECK_TRUE(RESULT("TAX #5").error             == assemble_error_bad_addressing_mode);
    RC_CHECK_TRUE(RESULT("{ LDA #0").error           == assemble_error_unclosed_scope);
}

RC_TEST_STEP(assemble, from_file, fix)
{
    // src/test/sample.6502 (copied next to the tests by CMake) holds "LDA #1 : RTS".
    assemble_result r = assemble_file(&fix->b, RC_STR("sample.6502"), fix->scratch);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x01, 0x60}, 3));

    RC_CHECK_TRUE(assemble_file(&fix->b, RC_STR("no_such_file.6502"), fix->scratch).error
                  == assemble_error_source_load);
}

RC_TEST_STEP(assemble, if_selects_branch, fix)
{
    // The active branch emits; the dead one is parsed for structure but produces nothing.
    RC_CHECK_TRUE(code_is(RESULT("LDA #0 : IF 1 : LDA #2 : ENDIF : LDA #3"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x02, 0xA9, 0x03}, 6));
    RC_CHECK_TRUE(code_is(RESULT("LDA #0 : IF 0 : LDA #2 : ENDIF : LDA #3"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x03}, 4));
}

RC_TEST_STEP(assemble, if_elif_else_chain, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("IF 1 : LDA #1 : ELIF 1 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x01}, 2));   // first true wins
    RC_CHECK_TRUE(code_is(RESULT("IF 0 : LDA #1 : ELIF 1 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x02}, 2));   // the elif
    RC_CHECK_TRUE(code_is(RESULT("IF 0 : LDA #1 : ELIF 0 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x03}, 2));   // the else
}

RC_TEST_STEP(assemble, if_does_not_introduce_scope, fix)
{
    // A label set in a live IF branch leaks to the enclosing scope (IF is not a brace).
    assemble_result r = RESULT("ORG &2000 : IF 1 : .here : ENDIF : LDA here");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xAD, 0x00, 0x20}, 3));
}

RC_TEST_STEP(assemble, if_wraps_scope_and_nests, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("IF 1 : { LDA #5 } : ENDIF"), (uint8_t[]){0xA9, 0x05}, 2));
    RC_CHECK(RESULT("IF 0 : { LDA #5 } : ENDIF").code.num, ==, 0u);
    RC_CHECK_TRUE(code_is(RESULT("IF 1 : IF 1 : LDA #7 : ENDIF : ENDIF"), (uint8_t[]){0xA9, 0x07}, 2));
    RC_CHECK(RESULT("IF 1 : IF 0 : LDA #7 : ENDIF : ENDIF").code.num, ==, 0u);
    RC_CHECK(RESULT("IF 0 : IF 1 : LDA #7 : ENDIF : ENDIF").code.num, ==, 0u);   // outer dead -> inner dead
}

RC_TEST_STEP(assemble, if_dead_branch_suppresses_value_errors_but_not_syntax, fix)
{
    // A dead branch raises no value/encoding errors...
    RC_CHECK_TRUE(RESULT("IF 0 : LDA undefined_thing : ENDIF").error == assemble_error_none);
    RC_CHECK_TRUE(RESULT("IF 0 : TAX #5 : ENDIF").error == assemble_error_none);
    // ...but a genuine parse error still aborts, even when dead.
    RC_CHECK_TRUE(RESULT("IF 0 : LDA #1 #2 : ENDIF").error == assemble_error_expected_separator);
}

RC_TEST_STEP(assemble, if_unknown_condition_defers, fix)
{
    // A forward-referenced condition is undecidable on the first pass: both branches stay inactive,
    // and a later pass takes the right one once it resolves.
    assemble_result r = RESULT("IF cond : LDA #1 : ENDIF\ncond = 1");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(r.passes >= 2);
}

RC_TEST_STEP(assemble, if_branch_flip_clears_label, fix)
{
    // `gate` is 1 on the pass after `.other` first appears, then 0 once defined(other) sees it - so
    // `.x` is bound on one pass and must be CLEARED when its branch goes inactive. The remove-on-
    // inactive rule leaves it undefined; a stale binding would linger.
    assemble_result r = RESULT("IF gate : .x : ENDIF\ngate = 1 - defined(other)\n.other");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("x"))));
}

RC_TEST_STEP(assemble, if_framing_errors, fix)
{
    RC_CHECK_TRUE(RESULT("IF 1 : LDA #1").error      == assemble_error_unclosed_if);   // no ENDIF
    RC_CHECK_TRUE(RESULT("{ IF 1 : LDA #1 }").error  == assemble_error_unclosed_if);   // '}' before ENDIF
    RC_CHECK_TRUE(RESULT("ENDIF").error              == assemble_error_unexpected_endif);
    RC_CHECK_TRUE(RESULT("ELIF 1 : ENDIF").error     == assemble_error_unexpected_elif);   // each closer its own error
    RC_CHECK_TRUE(RESULT("ELSE").error               == assemble_error_unexpected_else);
    RC_CHECK_TRUE(RESULT("IF 1 : ELIF 1 : ELIF 2 : ENDIF").error == assemble_error_none);    // ELIF chain is fine
    RC_CHECK_TRUE(RESULT("IF 1 : ELSE : ELIF 1 : ENDIF").error == assemble_error_unexpected_elif);   // ELIF after ELSE
    RC_CHECK_TRUE(RESULT("IF 1 : ELSE : ELSE : ENDIF").error == assemble_error_unexpected_else);     // second ELSE
    RC_CHECK_TRUE(RESULT("IF 0 : ELSE LDA #1 : ENDIF").error == assemble_error_expected_separator);   // ELSE needs a separator
}

// The next three exercise an IF condition that depends on a forward reference whose own size depends
// on the layout the condition controls - a fixed-point search over passes.

RC_TEST_STEP(assemble, if_forward_ref_condition_settles, fix)
{
    // `LDA fwdlabel` starts absolute (fwdlabel unknown), so the first guess lands fwdlabel at 6; once it
    // shrinks to zero-page, fwdlabel settles at 5, making `fwdlabel = 6` false - the block is skipped.
    assemble_result r = RESULT("LDA #1 : IF fwdlabel = 6 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x01, 0xEA, 0xA5, 0x05, 0x60}, 6));
}

RC_TEST_STEP(assemble, if_forward_ref_both_fixed_points_valid, fix)
{
    // Both "taken" (fwdlabel=10) and "skipped" (fwdlabel=5) are self-consistent fixed points. We
    // converge to whichever our first guess lands on: an unresolved operand resolves to zero-page
    // optimistically (smallest), so fwdlabel starts at 5, `5 > 5` is false, and we settle on skipped.
    assemble_result r = RESULT("LDA #1 : IF fwdlabel > 5 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x01, 0xEA, 0xA5, 0x05, 0x60}, 6));
}

RC_TEST_STEP(assemble, if_forward_ref_contradiction_does_not_converge, fix)
{
    // `fwdlabel = 5` is a contradiction: skipping the block puts fwdlabel at 5 (so the condition is
    // true, contradicting the skip); taking it puts fwdlabel at 10 (so the condition is false). The
    // layout flips between the two forever and never settles.
    assemble_result r = RESULT("LDA #1 : IF fwdlabel = 5 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS");
    RC_CHECK_TRUE(r.error == assemble_error_no_convergence);
}

RC_TEST_STEP(assemble, nested_if_forward_ref, fix)
{
    // Both an outer and an inner IF test the forward label `end`. The fixed code after the conditional
    // block (LDX/LDY) keeps `end` high: pass 1 emits nothing in the stuck branches, leaving end at 6,
    // which already clears both thresholds - so on the next pass both NOPs come in and end settles at 8.
    assemble_result r = RESULT("LDA #0 : IF end >= 4 : NOP : IF end >= 6 : NOP : ENDIF : ENDIF : LDX #1 : LDY #2 : .end : RTS");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x00, 0xEA, 0xEA, 0xA2, 0x01, 0xA0, 0x02, 0x60}, 9));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("end")), value_make_numeric(8)));
}

RC_TEST_STEP(assemble, org_from_forward_ref_ends_at_address, fix)
{
    // The "make this block end at address X" idiom: ORG is computed from a label defined AFTER it, so
    // the 8-byte block sits at &0FF8..&0FFF and progend lands exactly on &1000. ORG is unresolved on
    // the first pass (progstart/progend unknown) and settles once they do; JMP progstart then carries
    // the relocated address. (Code itself still fills the output from index 0.)
    assemble_result r = RESULT("ORG &1000 - (progend - progstart) : .progstart LDA #&41 : JSR &FFEE : JMP progstart : .progend");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xA9, 0x41, 0x20, 0xEE, 0xFF, 0x4C, 0xF8, 0x0F}, 8));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("progstart")), value_make_numeric(0x0FF8)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("progend")), value_make_numeric(0x1000)));
}

RC_TEST_STEP(assemble, for_repeats_body, fix)
{
    // FOR runs its body once per element, emitting a fresh copy each time.
    RC_CHECK_TRUE(code_is(RESULT("FOR i = 0..2 : NOP : NEXT"), (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
    // An exclusive range is the same minus its endpoint.
    RC_CHECK_TRUE(code_is(RESULT("FOR i = 0..<3 : NOP : NEXT"), (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
}

RC_TEST_STEP(assemble, for_binds_loop_variable, fix)
{
    // The loop variable holds the current element, visible to the body. 0..3 is inclusive (4 elements).
    RC_CHECK_TRUE(code_is(RESULT("FOR i = 0..3 : LDA #i : NEXT"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01, 0xA9, 0x02, 0xA9, 0x03}, 8));
    // A list literal drives the loop just as a range does.
    RC_CHECK_TRUE(code_is(RESULT("FOR x = {10, 20, 30} : LDA #x : NEXT"),
                          (uint8_t[]){0xA9, 0x0A, 0xA9, 0x14, 0xA9, 0x1E}, 6));
}

RC_TEST_STEP(assemble, for_iteration_has_its_own_scope, fix)
{
    // A label in the body is redefined every iteration; each iteration's private scope keeps that from
    // being a duplicate.
    assemble_result r = RESULT("FOR i = 0..2 : .lbl NOP : NEXT");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
}

RC_TEST_STEP(assemble, for_nests, fix)
{
    // Two iterations of two iterations: four bodies.
    RC_CHECK_TRUE(code_is(RESULT("FOR i = 0..1 : FOR j = 0..1 : NOP : NEXT : NEXT"),
                          (uint8_t[]){0xEA, 0xEA, 0xEA, 0xEA}, 4));
}

RC_TEST_STEP(assemble, for_empty_sequence_runs_zero_times, fix)
{
    // An empty sequence is legal and assembles the body zero times (no error): the surrounding code is
    // emitted as if the FOR were absent. Both the empty list literal and a non-ascending exclusive range.
    RC_CHECK_TRUE(code_is(RESULT("LDA #0 : FOR i = {} : NOP : NEXT : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
    RC_CHECK_TRUE(code_is(RESULT("LDA #0 : FOR i = 5..<5 : NOP : NEXT : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
}

RC_TEST_STEP(assemble, for_forward_ref_count_defers, fix)
{
    // When the count depends on a forward reference, the FOR runs zero times on the first pass and
    // re-expands once the bound resolves.
    assemble_result r = RESULT("FOR i = 0..n : NOP : NEXT\nn = 2");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
    RC_CHECK_TRUE(r.passes >= 2);
}

RC_TEST_STEP(assemble, for_framing_and_sequence_errors, fix)
{
    RC_CHECK_TRUE(RESULT("FOR i = 0..2 : NOP").error == assemble_error_unclosed_for);   // no NEXT
    RC_CHECK_TRUE(RESULT("NEXT").error                == assemble_error_unexpected_next);
    RC_CHECK_TRUE(RESULT("FOR i = 5 : NOP : NEXT").error  == assemble_error_not_iterable);   // a scalar is not a sequence
    RC_CHECK_TRUE(RESULT("FOR i = 5.. : NOP : NEXT").error == assemble_error_not_iterable);  // an unbounded range cannot be counted
}

RC_TEST_STEP(assemble, equb_emits_bytes, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("EQUB 1, 2, 3"), (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(RESULT("EQUB &FF"),     (uint8_t[]){0xFF}, 1));
    RC_CHECK_TRUE(code_is(RESULT("EQUB 255"),     (uint8_t[]){0xFF}, 1));
}

RC_TEST_STEP(assemble, equb_allows_signed_bytes, fix)
{
    // Negative numbers are allowed in -255..255; we emit the low byte.
    RC_CHECK_TRUE(code_is(RESULT("EQUB -1"),   (uint8_t[]){0xFF}, 1));
    RC_CHECK_TRUE(code_is(RESULT("EQUB -255"), (uint8_t[]){0x01}, 1));
    RC_CHECK_TRUE(RESULT("EQUB 256").error  == assemble_error_value_out_of_range);
    RC_CHECK_TRUE(RESULT("EQUB -256").error == assemble_error_value_out_of_range);
}

RC_TEST_STEP(assemble, equs_and_equb_are_aliases, fix)
{
    // EQUS spells out a string char-by-char...
    RC_CHECK_TRUE(code_is(RESULT("EQUS \"ABC\""), (uint8_t[]){0x41, 0x42, 0x43}, 3));
    // ...but takes numbers too, and EQUB takes strings - they are the same directive.
    RC_CHECK_TRUE(code_is(RESULT("EQUS 65, 66"),  (uint8_t[]){0x41, 0x42}, 2));
    RC_CHECK_TRUE(code_is(RESULT("EQUB \"Hi\", 0"), (uint8_t[]){0x48, 0x69, 0x00}, 3));
}

RC_TEST_STEP(assemble, equb_ranges_and_lists, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("EQUB 1..3"),  (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(RESULT("EQUB 0..<3"), (uint8_t[]){0x00, 0x01, 0x02}, 3));
    // A list is flattened (nested lists and ranges descend); a string element stays whole.
    RC_CHECK_TRUE(code_is(RESULT("EQUB {1, 2, 3}"),       (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(RESULT("EQUB {1, {2, 3}, 4}"),  (uint8_t[]){0x01, 0x02, 0x03, 0x04}, 4));
    RC_CHECK_TRUE(code_is(RESULT("EQUB {1, \"Hi\", 2}"),  (uint8_t[]){0x01, 0x48, 0x69, 0x02}, 4));
}

RC_TEST_STEP(assemble, equb_advances_pc, fix)
{
    // The three bytes move pc, so a label after them sees the advanced address.
    assemble_result r = RESULT("ORG &2000 : EQUB 1, 2, 3 : .here");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")), value_make_numeric(0x2003)));
}

RC_TEST_STEP(assemble, equb_forward_reference, fix)
{
    // A forward reference is one byte (a placeholder until it resolves): end - start = 1.
    assemble_result r = RESULT(".start EQUB end - start : .end");
    RC_CHECK_TRUE(r.error == assemble_error_none);
    RC_CHECK_TRUE(code_is(r, (uint8_t[]){0x01}, 1));
    RC_CHECK_TRUE(r.passes >= 2);
}

RC_TEST_STEP(assemble, equb_dead_branch_emits_nothing, fix)
{
    RC_CHECK_TRUE(code_is(RESULT("LDA #0 : IF 0 : EQUB 1, 2, 3 : ENDIF : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
}

#endif // BARON_TESTS
