#include "assemble_internal.h"   // the parsing vocabulary (and assemble.h via it)

#include "opcodes.h"
#include "lexer.h"
#include "expression.h"
#include "file_utils.h"          // INCLUDE's path resolution
#include "baron.h"               // the owner type the tests assemble into
#include "richc/macros.h"


#define ASSEMBLE_MAX_PASSES 100u
#define ASSEMBLE_MAX_INCLUDE_DEPTH 64u   // a runaway / cyclic INCLUDE is caught here before the C stack gives out


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
static parse_result handle_include(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_reserved_constant(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result parse_block(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result parse_file(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
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

    if (value_is_error(v) && v.error == error_type_unknown_symbol) {
        if (final_pass) {
            return (int_argument) {
                .type = int_argument_type_error,
                .error = error_type_undefined_symbol,
                .error_at = at
            };
        }
        return (int_argument) { .type = int_argument_type_unresolved };   // a forward reference; settles later
    }

    // Any other error value carries its specific cause through unchanged (value and the assembler share
    // one error_type space); a non-error non-number (a string, a list) is simply not an address.
    return (int_argument) {
        .type = int_argument_type_error,
        .error = value_is_error(v) ? v.error : error_type_operand_not_numeric,
        .error_at = at
    };
}


// The two ways a parse reports an error - both record straight into b->diagnostics, so the code and
// location never travel in the parse_result. syntax_error unwinds (the stream is broken); semantic_error
// carries on (only recording on the settling pass of a live branch). Shared with opcodes.c.
parse_result syntax_error(baron *b, error_type code, cursor at)
{
    baron_error(b, code, at);
    return (parse_result) {.next = at.pos, .fatal = true};
}

void semantic_error(baron *b, parse_flags flags, error_type code, cursor at)
{
    if (flags.final && flags.active) {
        baron_error(b, code, at);
    }
}

void semantic_warning(baron *b, parse_flags flags, error_type code, cursor at, uint8_t severity)
{
    if (flags.final && flags.active) {
        baron_warning(b, code, at, severity);
    }
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
    {RC_STR("include"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_include}}},
    // The pure expression constants are reserved at statement start too, so `pi = 5` is rejected rather than
    // quietly binding a shadowed symbol. Three near-identical rows, but it is only three tokens.
    {RC_STR("true"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR("false"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR("pi"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR("elif"),   {.type = lexeme_type_closer, .closer = {closer_elif,  error_type_unexpected_elif}}},
    {RC_STR("else"),   {.type = lexeme_type_closer, .closer = {closer_else,  error_type_unexpected_else}}},
    {RC_STR("endif"),  {.type = lexeme_type_closer, .closer = {closer_endif, error_type_unexpected_endif}}},
    {RC_STR("next"),   {.type = lexeme_type_closer, .closer = {closer_next,  error_type_unexpected_next}}},
    {RC_STR("}"),      {.type = lexeme_type_closer, .closer = {closer_brace, error_type_unexpected_close_brace}}},
};
static const token_table statement_tokens = RC_VIEW(statement_token_entries);

// require_separator is shared with opcodes.c (declared in assemble.h); it reads the statement
// table to recognise the '}' that implicitly closes a one-liner, so it lives here with it.
parse_result require_separator(baron *b, cursor at)
{
    rc_str source = source_files_text(&b->source_files, at.source);
    lexer_result r = lexer_next(source, at.pos, statement_tokens);

    if (r.token.type == lexeme_type_terminator) {
        return (parse_result) { .next = r.next };
    }

    if (r.token.type == lexeme_type_closer && r.token.closer.id == closer_brace) {
        return (parse_result) { .next = at.pos };   // '}' closes the statement; parse_block consumes it
    }

    return syntax_error(b, error_type_expected_separator, at);
}

// The one place that projects baron into an expr_env: symbols from `scope`, the live PC of the current
// overlay. Every directive / operand evaluates through here, so no call site rebuilds the environment and
// the expression parser never sees baron. Shared with opcodes.c.
expr_result eval(baron *b, rc_str src, uint32_t pos, uint32_t scope, rc_arena scratch)
{
    expr_env env = {
        .scopes      = &b->scopes,
        .scope_index = scope,
        .pc          = overlays_pc(&b->overlays, b->current_overlay),
    };
    return expression_parse(src, pos, &env, &scratch);
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

// Fold a sub-parse's outcome into r: take its pos and OR its flags. Errors were already recorded
// into b->diagnostics at the failure site, so there is nothing to carry but `fatal` - which, once
// set, tells the statement loop to unwind.
static parse_result fold(parse_result r, parse_result sub)
{
    r.next        = sub.next;
    r.unresolved |= sub.unresolved;
    r.changed    |= sub.changed;
    r.fatal      |= sub.fatal;
    return r;
}


// ---- directive statements ----

static parse_result handle_org(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = eval(b, src, at.pos, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    // A dead branch parses the operand but applies nothing - all the effect lives in this block.
    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_known:
                overlays_org(&b->overlays, b->current_overlay, (uint32_t) (arg.value & 0xFFFF));
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown ORG leaves pc as-is this pass; forces another pass
                break;
            case int_argument_type_error:
                semantic_error(b, flags, arg.error, cursor_at(at, arg.error_at));   // leave pc as-is
                break;
        }
    }

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// SKIP n - pad the object code with n zero bytes (advancing pc by n). A negative count would
// rewind the pointer, which we cannot do; the layout-dependent check is deferred to the final pass.
static parse_result handle_skip(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = eval(b, src, at.pos, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_known:
                if (arg.value < 0) {
                    semantic_error(b, flags, error_type_skip_backwards, cursor_at(at, at.pos));   // skip nothing
                }
                else {
                    overlays_skip(&b->overlays, b->current_overlay, (uint32_t) arg.value);
                }
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown count emits nothing; forces another pass
                break;
            case int_argument_type_error:
                semantic_error(b, flags, arg.error, cursor_at(at, arg.error_at));   // skip nothing
                break;
        }
    }

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// SKIPTO addr - pad with zeroes until pc reaches addr. Being already past addr is an error,
// deferred to the final pass since pc only settles once preceding forward references resolve.
static parse_result handle_skipto(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = eval(b, src, at.pos, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_known: {
                uint32_t pc = overlays_pc(&b->overlays, b->current_overlay);
                if (arg.value < pc) {
                    semantic_error(b, flags, error_type_skip_backwards, cursor_at(at, at.pos));   // skip nothing
                }
                else {
                    overlays_skip(&b->overlays, b->current_overlay, (uint32_t) (arg.value - pc));
                }
                break;
            }
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown target emits nothing; forces another pass
                break;
            case int_argument_type_error:
                semantic_error(b, flags, arg.error, cursor_at(at, arg.error_at));   // skip nothing
                break;
        }
    }

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// ALIGN n - pad with zeroes until pc is a multiple of n. n < 1 is meaningless (and would divide
// by zero), so it is an error; the modulo is only evaluated once we know n is sound.
static parse_result handle_align(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = eval(b, src, at.pos, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_known:
                if (arg.value < 1) {
                    semantic_error(b, flags, error_type_bad_alignment, cursor_at(at, at.pos));   // pad nothing
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
            case int_argument_type_error:
                semantic_error(b, flags, arg.error, cursor_at(at, arg.error_at));   // pad nothing
                break;
        }
    }

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// Emit one value's bytes into the current overlay, for EQUB / EQUS. A string goes character by
// character; a range is enumerated; a list is descended (so nested lists and ranges flatten out);
// anything else is a single byte via int_argument_make, where a forward reference emits a 0 placeholder
// and asks for another pass. Returns .error/.error_at on failure and .unresolved when a value defers
// (its .next is unused). A dead branch emits nothing and raises nothing - mirroring inactive statements.
static parse_result emit_data(baron *b, value v, parse_flags flags, cursor at, rc_arena scratch)
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
            acc = fold(acc, emit_data(b, rc_view_value_get(v.list, i), flags, at, scratch));
        }
        return acc;
    }

    // A numeric, a forward reference, or some other error value - one byte either way.
    int_argument arg = int_argument_make(v, flags.final, at.pos);
    if (arg.type == int_argument_type_error) {
        semantic_error(b, flags, arg.error, at);
        overlays_emit_u8(&b->overlays, b->current_overlay, 0);   // best-effort placeholder; keeps the size stable
        return (parse_result) {0};
    }
    if (arg.type == int_argument_type_unresolved) {
        overlays_emit_u8(&b->overlays, b->current_overlay, 0);   // placeholder; its size is assumed one byte
        return (parse_result) {.unresolved = true};
    }
    // We allow signed bytes, so the byte-sized window is -255..255; recorded only once everything settles.
    if (arg.value < -255 || arg.value > 255) {
        semantic_error(b, flags, error_type_value_out_of_range, at);
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
        expr_result e = eval(b, src, pos, scope, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        parse_result em = emit_data(b, e.value, flags, cursor_at(at, pos), scratch);
        if (em.fatal) {
            return em;
        }
        unresolved |= em.unresolved;

        lexer_result lr = lexer_next(src, e.next, statement_tokens);
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        parse_result r = require_separator(b, cursor_at(at, e.next));
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
    if (nm.token.type != lexeme_type_identifier) {
        return syntax_error(b, error_type_expected_label_name, cursor_at(at, at.pos));   // no name token: malformed
    }
    rc_str name = nm.token.identifier.name;

    parse_result r = {.next = nm.next};

    // A dotted name is a legal token but not a legal label - the stream is fine, so we record it and
    // carry on (consuming just the name, leaving the rest of the line to the loop).
    if (is_dotted(name)) {
        semantic_error(b, flags, error_type_expected_label_name, cursor_at(at, at.pos));
        return r;
    }

    // Bind name = current pc in the current scope; a moved label drives another pass, and a second
    // definition of the same name here (a different source position) is a duplicate - recorded, but the
    // first binding stands and we press on. In a dead branch we instead REMOVE the name, clearing any
    // binding a live earlier pass left behind.
    if (flags.active) {
        symbol_status st = scopes_set_symbol(
            &b->scopes,
            scope,
            name,
            value_make_numeric((double)overlays_pc(&b->overlays, b->current_overlay)),
            at
        );

        if (st == symbol_status_duplicate) {
            semantic_error(b, flags, error_type_duplicate_symbol, cursor_at(at, at.pos));
            cursor original = scopes_symbol_def(&b->scopes, scope, name);
            if (!cursor_is_none(original)) {
                semantic_error(b, flags, error_type_original_definition, original);   // point at the first binding
            }
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

    expr_result e = eval(b, src, at.pos, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));   // a syntax error always aborts
    }

    parse_result sep = require_separator(b, cursor_at(at, e.next));
    if (sep.fatal) {
        return sep;
    }

    // Judge the condition only when the parent is live. if_cond and else_cond are mutually exclusive
    // (and both false when the condition is unevaluable or in error - then neither branch is live).
    bool if_cond = false;
    bool else_cond = false;
    parse_result acc = {.next = sep.next};

    if (flags.active) {
        int_argument cond = int_argument_make(e.value, flags.final, at.pos);
        switch (cond.type) {
            case int_argument_type_known:
                if_cond = (cond.value != 0);
                else_cond = !if_cond;
                break;
            case int_argument_type_unresolved:
                acc.unresolved = true;   // undecidable yet; owe another pass
                break;
            case int_argument_type_error:
                semantic_error(b, flags, cond.error, cursor_at(at, cond.error_at));   // neither branch runs
                break;
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

    if (acc.fatal) {
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
        parse_result sep = require_separator(b, cursor_at(at, t.next));
        if (sep.fatal) {
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

        if (acc.fatal) {
            return acc;
        }

        // Now read the token after the ELSE block, which must be ENDIF. The ELSE body is always the last
        t = lexer_next(src, acc.next, statement_tokens);
    }

    // If it was ENDIF, close the IF block here
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_endif) {
        acc.next = t.next;
        return fold(acc, require_separator(b, cursor_at(at, acc.next)));
    }

    // A chain keyword out of place (a second ELSE, an ELIF after ELSE) is unexpected - raise its own
    // error; any other closer ('}', NEXT) or end of input means there was no ENDIF at all. Either way
    // the block structure is broken, so this is fatal.
    error_type code = t.token.type == lexeme_type_closer
                              && (t.token.closer.id == closer_elif || t.token.closer.id == closer_else)
                          ? (error_type) t.token.closer.unexpected
                          : error_type_unclosed_if;
    return fold(acc, syntax_error(b, code, cursor_at(at, acc.next)));
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

    lexer_result eq = lexer_next(src, at.pos, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return syntax_error(b, error_type_expected_assign, cursor_at(at, at.pos));   // not an assignment: malformed
    }

    expr_result e = eval(b, src, eq.next, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    // The statement is fully consumed now, so any problem with it is recoverable. A dotted target is a
    // legal token but not a legal binding name: record it and bind nothing. Otherwise bind name = value;
    // a second definition of the same name (a different source position) is a duplicate - recorded, first
    // binding stands. In a dead branch we instead REMOVE the name, clearing a binding a live earlier pass
    // left behind, and raise none of the value-dependent errors.
    parse_result r = {.next = e.next};
    if (is_dotted(name)) {
        semantic_error(b, flags, error_type_invalid_assignment, cursor_at(at, at.pos));
    }
    else if (flags.active) {
        symbol_status st = scopes_set_symbol(&b->scopes, scope, name, e.value, at);

        if (st == symbol_status_duplicate) {
            semantic_error(b, flags, error_type_duplicate_symbol, cursor_at(at, at.pos));
            cursor original = scopes_symbol_def(&b->scopes, scope, name);
            if (!cursor_is_none(original)) {
                semantic_error(b, flags, error_type_original_definition, original);   // point at the first binding
            }
        }
        else {
            if (value_is_error(e.value)) {
                if (e.value.error == error_type_unknown_symbol) {
                    semantic_error(b, flags, error_type_undefined_symbol, cursor_at(at, eq.next));
                    if (!flags.final) r.unresolved = true;   // a forward reference in the value; settles on a later pass
                }
                else {
                    semantic_error(b, flags, e.value.error, cursor_at(at, eq.next));   // e.g. x = 1/0
                }
            }
            r.changed = (st == symbol_status_changed);
        }
    }
    else {
        r.changed = scopes_remove_symbol(&b->scopes, scope, name);
    }

    return fold(r, require_separator(b, cursor_at(at, e.next)));
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

    // The loop variable: a bare identifier (a dotted path cannot be a binding target). A malformed
    // header (no name, no '=') is fatal - we cannot reliably find the matching NEXT.
    lexer_result var = lexer_next(src, at.pos, statement_tokens);
    if (var.token.type != lexeme_type_identifier || is_dotted(var.token.identifier.name)) {
        return syntax_error(b, error_type_expected_label_name, cursor_at(at, at.pos));
    }
    rc_str name = var.token.identifier.name;

    lexer_result eq = lexer_next(src, var.next, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return syntax_error(b, error_type_expected_assign, cursor_at(at, var.next));
    }

    expr_result e = eval(b, src, eq.next, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    parse_result sep = require_separator(b, cursor_at(at, e.next));
    if (sep.fatal) {
        return sep;
    }
    uint32_t body_start = sep.next;

    // Reduce the sequence to its element list, but only when the parent is live (a dead FOR evaluates
    // nothing, like IF). iterate stays false for an empty list, an unresolved count, a bad sequence, or a
    // dead parent - all of which run the body once inactive (which is also how we locate NEXT). A bad
    // sequence (not a list or range) is recorded but recoverable: we press on and close the loop.
    bool iterate = false;
    bool unresolved = false;
    rc_view_value items = {0};
    if (flags.active) {
        value seq = e.value;
        if (value_is_error(seq)) {
            if (seq.error == error_type_unknown_symbol) {
                semantic_error(b, flags, error_type_undefined_symbol, cursor_at(at, eq.next));
                if (!flags.final) unresolved = true;   // the count is not known yet; defer and try again next pass
            }
            else {
                semantic_error(b, flags, seq.error, cursor_at(at, eq.next));   // e.g. the sequence expression divided by zero
            }
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
                semantic_error(b, flags, error_type_not_iterable, cursor_at(at, eq.next));   // scalar/string/none, or an unbounded range
            }
        }
    }

    // Run the body: once per element when iterating, otherwise a single inactive structural pass (which
    // is also how we locate NEXT). Every pass re-walks the same body span and returns .next at NEXT.
    uint32_t count = iterate ? items.num : 1;
    bool active_body = iterate;   // iterate implies flags.active; empty / unresolved / dead parse inactive
    parse_result acc = {.next = body_start, .unresolved = unresolved};
    for (uint32_t i = 0; i < count && !acc.fatal; i++) {
        char storage[64];
        rc_mstr key = anon_for_scope_key(storage, sizeof storage, at, i);
        uint32_t child = scopes_get_or_make_child(&b->scopes, scope, key.view);
        if (iterate) {
            scopes_set_symbol(&b->scopes, child, name, rc_view_value_get(items, i), at);
        }
        acc = fold(acc, parse_block(b, cursor_at(at, body_start), child,
                                    (parse_flags) {flags.final, active_body}, scratch));
    }
    if (acc.fatal) {
        return acc;
    }

    // Close with NEXT, which insists on a separator after it (like ENDIF).
    lexer_result t = lexer_next(src, acc.next, statement_tokens);
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_next) {
        acc.next = t.next;
        return fold(acc, require_separator(b, cursor_at(at, acc.next)));
    }
    return fold(acc, syntax_error(b, error_type_unclosed_for, cursor_at(at, acc.next)));   // '}' / EOF / foreign keyword before NEXT
}


// ---- INCLUDE ----

// INCLUDE "file" splices another source in at this point - textually, so its code emits into the current
// overlay at the current pc and its symbols bind into the current scope (no scope of its own). We re-parse
// the included file every pass, exactly like the rest of the statement stream, so forward references cross
// the boundary freely. The filename is resolved relative to THIS file's directory (see file_path_resolve).
static parse_result handle_include(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The filename is a string operand, evaluated on the spot - we load the file this very pass, so a
    // forward-referenced name is no use to us (it stays an error value, and we grumble about it below).
    expr_result e = eval(b, src, at.pos, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    // The pull-in's own outcome: it stays empty unless we actually load a file. A dead branch swallows the
    // filename and the separator but pulls in nothing at all.
    parse_result pulled = {0};
    if (flags.active) {
        if (value_is_string(e.value)) {
            if (b->include_depth >= ASSEMBLE_MAX_INCLUDE_DEPTH) {
                semantic_error(b, flags, error_type_include_too_deep, cursor_at(at, at.pos));
            }
            else {
                rc_str base = source_files_name(&b->source_files, at.source);
                rc_str path = file_path_resolve(base, e.value.string, &scratch);   // source_files keeps its own copy
                uint32_t inc_source = source_files_add_file(&b->source_files, path);
                if (inc_source == RC_INDEX_NONE) {
                    semantic_error(b, flags, error_type_source_load, cursor_at(at, at.pos));
                }
                else {
                    uint32_t errors_before = baron_error_count(b);
                    b->include_depth++;
                    pulled = parse_file(b, (cursor) {.source = inc_source, .pos = 0}, scope, flags, scratch);
                    b->include_depth--;
                    if (pulled.fatal) {
                        return pulled;   // a broken statement stream in the included file aborts the whole assemble
                    }
                    // If the file we just pulled in raised any errors, drop a breadcrumb pointing back at this
                    // INCLUDE. It lands AFTER the file's own errors, so the list reads innermost-first.
                    if (baron_error_count(b) > errors_before) {
                        semantic_error(b, flags, error_type_included_from, cursor_at(at, at.pos));
                    }
                }
            }
        }
        else if (value_is_error(e.value) && e.value.error == error_type_unknown_symbol) {
            // A forward-referenced filename: defer, exactly like a forward address. We pull in nothing this
            // pass; once the name binds on a later pass the string resolves and the file loads. Still unknown
            // on the final pass means it never will be (a name defined only inside the file it would name), so
            // we call it out then.
            if (flags.final) {
                semantic_error(b, flags, error_type_undefined_symbol, cursor_at(at, at.pos));
            }
            else {
                pulled.unresolved = true;
            }
        }
        else {
            // A number, a list, some other eval error: this is never going to name a file.
            semantic_error(b, flags, error_type_expected_filename, cursor_at(at, at.pos));
        }
    }

    // Carry the pull-in's unresolved/changed up so the driver runs another pass if it must, and carry on in
    // THIS file at the separator just past the filename.
    return fold(pulled, require_separator(b, cursor_at(at, e.next)));
}


// TRUE / FALSE / PI are expression constants, reserved so that a name always resolves to the constant.
// Meeting one at statement start is someone assigning to it (pi = 5) or otherwise misusing it as a name -
// a fatal error, the same way a name that clashes with a mnemonic is rejected.
static parse_result handle_reserved_constant(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    (void) scope;
    (void) flags;
    (void) scratch;
    return syntax_error(b, error_type_reserved_constant, cursor_at(at, at.pos));
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
            return syntax_error(b, error_type_unexpected_token, at);
    }
}

// Parse statements until the block's closer: a '}' or end of input. It stops AT the closer -
// leaving a '}' unconsumed - and treats neither as an error, because which closer is required
// depends on the caller: parse_scope wants the '}', run_pass wants end of input.
static parse_result parse_block(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    parse_result acc = {.next = at.pos};
    while (!acc.fatal) {
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

    if (r.fatal) {
        return r;
    }

    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, r.next, statement_tokens);

    if (lr.token.type == lexeme_type_closer) {
        if (lr.token.closer.id == closer_brace) {   // the '}' we were waiting for
            r.next = lr.next;
            return r;
        }
        return fold(r, syntax_error(b, (error_type) lr.token.closer.unexpected, cursor_at(at, lr.next)));   // an IF/FOR closer with nothing to close
    }

    return fold(r, syntax_error(b, error_type_unclosed_scope, cursor_at(at, r.next)));   // end of input before the '}'
}


// A whole source file - the largest parsable item: parse its statements and require they run right
// out at end of input. parse_block stops only at a block closer or at end of input, so a leftover
// closer here has nothing to close: a stray '}', an IF-chain keyword with no IF, or a NEXT with no
// FOR. With those ruled out, end of input is the only thing left (asserted). (parse_scope is a braced
// block within a file; parse_block is the shared statement loop both rest on.)
static parse_result parse_file(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    parse_result r = parse_block(b, at, scope, flags, scratch);

    if (r.fatal) {
        return r;
    }

    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, r.next, statement_tokens);

    if (lr.token.type == lexeme_type_closer) {   // any closer at file scope has nothing to close
        return fold(r, syntax_error(b, (error_type) lr.token.closer.unexpected, cursor_at(at, lr.next)));
    }

    RC_ASSERT(lexer_at_end(src, r.next));   // parse_block had no other reason to stop
    return r;
}


// ---- the multi-pass driver ----

static parse_result run_pass(baron *b, uint32_t source, parse_flags flags, rc_arena scratch)
{
    overlays_reset_all(&b->overlays);
    b->current_overlay = overlays_default;   // each pass re-derives the current overlay from the source
    b->include_depth   = 0;                  // balanced by handle_include, but a fatal unwind skips the decrement

    const uint32_t scope = 0;
    return parse_file(
        b,
        (cursor) {.source = source, .pos = 0},
        scope,
        flags,
        scratch
    );
}

// Discard the half-built outputs - object code and symbols - so a failed assemble hands back nothing
// to consume; only the diagnostics remain. Returns 0, the failure signal the entry points hand back.
static uint32_t assemble_failed(baron *b)
{
    overlays_reset_all(&b->overlays);
    scopes_reset(&b->scopes);
    return 0;
}

// The shared core: run passes over the source already cached at index `source` in b, until the labels
// and forward references settle (or non-convergence). Returns the pass count, or 0 on failure (which
// clears the outputs). Diagnostics were reset by the entry point; only the single settling / diagnostic
// pass (flags.final) records recoverable errors and warnings, so the array holds the complete final set.
// assemble_string / assemble_file are thin wrappers that establish the source entry, then call this.
static uint32_t run_passes(baron *b, uint32_t source, rc_arena scratch)
{
    for (uint32_t pass = 1; pass <= ASSEMBLE_MAX_PASSES; pass++) {
        parse_result r = run_pass(b, source, (parse_flags) {.active = true}, scratch);

        if (r.fatal) {
            return assemble_failed(b);   // a syntax error broke the token stream
        }

        if (!r.unresolved && !r.changed) {
            // Settled. One more pass with checks armed, to record any deferred (recoverable) errors and
            // warnings. It fails only on an error - warnings leave the assemble succeeding.
            parse_result fin = run_pass(b, source, (parse_flags) {.final = true, .active = true}, scratch);

            if (fin.fatal || baron_has_errors(b)) {
                return assemble_failed(b);
            }
            return pass + 1;
        }
    }

    // Did not settle within the cap. A final diagnostic pass names a concrete cause (an undefined
    // symbol) where there is one; otherwise it is genuine oscillation.
    parse_result diag = run_pass(b, source, (parse_flags) {.final = true, .active = true}, scratch);
    if (!diag.fatal && !baron_has_errors(b)) {
        baron_error(b, error_type_no_convergence, (cursor) {.source = source, .pos = 0});
    }
    return assemble_failed(b);
}

uint32_t assemble_string(baron *b, rc_str name, rc_str text, rc_arena scratch)
{
    RC_ASSERT(b != NULL);
    rc_array_diagnostic_reset(&b->diagnostics);
    return run_passes(b, source_files_add_string(&b->source_files, name, text), scratch);
}

uint32_t assemble_file(baron *b, rc_str path, rc_arena scratch)
{
    RC_ASSERT(b != NULL);
    rc_array_diagnostic_reset(&b->diagnostics);
    uint32_t source = source_files_add_file(&b->source_files, path);
    if (source == RC_INDEX_NONE) {
        baron_error(b, error_type_source_load, (cursor) {0});
        return assemble_failed(b);
    }
    return run_passes(b, source, scratch);
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

// Name each test source after its own text, so distinct snippets get distinct source-cache entries
// (the cache is keyed by name) even though they share one baron across the step. ASM assembles a
// snippet and yields the pass count (0 on failure); object code, symbols and diagnostics are then read
// back from fix->b.
#define ASM(src) assemble_string(&fix->b, RC_STR(src), RC_STR(src), fix->scratch)

// The default overlay's current object code.
static rc_view_bytes obj(baron *b)
{
    return overlays_code(&b->overlays, overlays_default);
}

// Whether a clean assemble (passes != 0) laid down exactly these bytes.
static bool code_is(baron *b, uint32_t passes, const uint8_t *exp, uint32_t n)
{
    rc_view_bytes code = obj(b);
    if (passes == 0 || code.num != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (rc_view_bytes_get(code, i) != exp[i]) {
            return false;
        }
    }
    return true;
}

// The first error-severity diagnostic's code (error_type_none if the assemble raised no error).
// Warnings are skipped, so a snippet that succeeds with only warnings still reports none.
static error_type first_error(baron *b)
{
    for (uint32_t i = 0; i < b->diagnostics.num; i++) {
        diagnostic d = rc_view_diagnostic_get(b->diagnostics.view, i);
        if (d.severity == severity_error) {
            return d.code;
        }
    }
    return error_type_none;
}

// Assemble and yield that first error code - the common "what error did this snippet raise?" check.
#define ERR(src) (ASM(src), first_error(&fix->b))

// Whether any recorded diagnostic - of any severity - carries this code.
static bool has_diag(baron *b, error_type code)
{
    for (uint32_t i = 0; i < b->diagnostics.num; i++) {
        if (rc_view_diagnostic_get(b->diagnostics.view, i).code == code) {
            return true;
        }
    }
    return false;
}

RC_TEST_STEP(assemble, addressing_modes, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #0"),       (uint8_t[]) {0xA9, 0x00}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA &70"),      (uint8_t[]) {0xA5, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA &2000"),    (uint8_t[]) {0xAD, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA &70,X"),    (uint8_t[]) {0xB5, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA &2000,X"),  (uint8_t[]) {0xBD, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDX &70,Y"),    (uint8_t[]) {0xB6, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA (&70),Y"),  (uint8_t[]) {0xB1, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("STA (&70,X)"),  (uint8_t[]) {0x81, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("JMP (&1234)"),  (uint8_t[]) {0x6C, 0x34, 0x12}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("JMP &70"),      (uint8_t[]) {0x4C, 0x70, 0x00}, 3));   // no zp JMP
    RC_CHECK_TRUE(code_is(&fix->b, ASM("ASL"),          (uint8_t[]) {0x0A}, 1));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("ASL A"),        (uint8_t[]) {0x0A}, 1));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("NOP"),          (uint8_t[]) {0xEA}, 1));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("STA &70"),      (uint8_t[]) {0x85, 0x70}, 2));
}

RC_TEST_STEP(assemble, multiple_statements, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #1 : STA &70 : RTS"),
                          (uint8_t[]){0xA9, 0x01, 0x85, 0x70, 0x60}, 5));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #1\nSTA &70\nRTS"),
                          (uint8_t[]){0xA9, 0x01, 0x85, 0x70, 0x60}, 5));
}

RC_TEST_STEP(assemble, branch_offsets, fix)
{
    // Backward: target at pc 0, NOP, then BNE back to it. offset = 0 - (1 + 2) = -3 = 0xFD.
    RC_CHECK_TRUE(code_is(&fix->b, ASM(".t NOP : BNE t"), (uint8_t[]){0xEA, 0xD0, 0xFD}, 3));
    // Forward: BEQ over a following NOP. BEQ at 0, NOP at 2, target = 3, offset = 3 - 2 = 1.
    // (`over` not `skip`: SKIP is now a reserved directive keyword, like a mnemonic.)
    RC_CHECK_TRUE(code_is(&fix->b, ASM("BEQ over : NOP : .over"), (uint8_t[]){0xF0, 0x01, 0xEA}, 3));
}

RC_TEST_STEP(assemble, branch_to_shadowed_forward_label, fix)
{
    // `.label` names the scope, and an inner `.label` shadows it. On the first pass the inner label
    // is not yet bound, so BEQ transiently resolves to the FAR outer label (256+ bytes back) - a
    // branch that would be out of range. That error must be withheld on non-final passes; by the
    // time the layout settles the near inner label is bound and the branch is in range (offset 0).
    uint32_t passes = ASM(".label { SKIP 256 : BEQ label : .label }");
    RC_CHECK_TRUE(passes != 0);
    rc_view_bytes code = obj(&fix->b);
    RC_CHECK(code.num, ==, 258u);                                  // 256 skipped + BEQ (2 bytes)
    RC_CHECK((uint32_t)rc_view_bytes_get(code, 256), ==, 0xF0u);   // BEQ opcode
    RC_CHECK((uint32_t)rc_view_bytes_get(code, 257), ==, 0x00u);   // resolves near: branch to the next byte
}

RC_TEST_STEP(assemble, skip_skipto_align, fix)
{
    // SKIP n emits n zero bytes.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #1 : SKIP 3 : RTS"), (uint8_t[]) {0xA9, 0x01, 0x00, 0x00, 0x00, 0x60}, 6));

    // SKIPTO addr fills with zeroes until pc reaches addr (0x2001 -> 0x2004 = three bytes).
    RC_CHECK_TRUE(code_is(&fix->b, ASM("ORG &2000 : NOP : SKIPTO &2004 : RTS"), (uint8_t[]) {0xEA, 0x00, 0x00, 0x00, 0x60}, 5));
    // Already past addr is an error.
    RC_CHECK_TRUE(ERR("ORG &2000 : NOP : NOP : SKIPTO &2001") == error_type_skip_backwards);

    // ALIGN n pads up to the next multiple of n (pc 1 -> 4 = three bytes)...
    RC_CHECK_TRUE(code_is(&fix->b, ASM("NOP : ALIGN 4 : RTS"), (uint8_t[]) {0xEA, 0x00, 0x00, 0x00, 0x60}, 5));
    // ...and is a no-op when pc already sits on the boundary.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("ALIGN 4 : NOP"), (uint8_t[]) {0xEA}, 1));
    // ALIGN 0 is meaningless.
    RC_CHECK_TRUE(ERR("ALIGN 0") == error_type_bad_alignment);
}

RC_TEST_STEP(assemble, skip_advances_pc, fix)
{
    // SKIP moves pc, so a label after it sees the advanced address.
    uint32_t passes = ASM("ORG &2000 : .a SKIP 4 : .b");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("a")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("b")), value_make_numeric(0x2004)));
}

RC_TEST_STEP(assemble, org_and_labels, fix)
{
    // ORG sets the label's value but not where code lands (code still fills from index 0).
    uint32_t passes = ASM("ORG &2000 : LDA #1 : .here");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")),
                                 value_make_numeric(0x2002)));
}

RC_TEST_STEP(assemble, symbol_definition, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("n = 5 : LDA #n"), (uint8_t[]){0xA9, 0x05}, 2));
}

RC_TEST_STEP(assemble, duplicate_symbol_is_rejected, fix)
{
    // Two definitions of one name in the same scope - whether labels or assignments - are a
    // duplicate. Distinct names per snippet, since these share one baron's persistent scopes.
    RC_CHECK_TRUE(ERR(".dup NOP : .dup RTS") == error_type_duplicate_symbol);
    RC_CHECK_TRUE(ERR("twice = 1 : twice = 2") == error_type_duplicate_symbol);
}

RC_TEST_STEP(assemble, inner_scope_mirrors_outer_name, fix)
{
    // The same name in a nested scope is fine: the inner binding lives in its own scope.
    RC_CHECK_TRUE(ERR("x = 1 : { x = 2 }") == error_type_none);
}

RC_TEST_STEP(assemble, forward_reference_is_order_independent, fix)
{
    // A forward reference to a zero-page value converges to zero-page, the same as if it had
    // been defined first.
    uint32_t passes = ASM("LDA foo : foo = &70");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA5, 0x70}, 2));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, forward_reference_to_absolute, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA foo : foo = &2000"), (uint8_t[]){0xAD, 0x00, 0x20}, 3));
}

RC_TEST_STEP(assemble, named_scope_dotted_access, fix)
{
    RC_CHECK_TRUE(ASM("ORG &2000 : .routine { .core LDA #0 } : x = routine.core") != 0);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("x")),
                                 value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("routine.core")),
                                 value_make_numeric(0x2000)));
}

RC_TEST_STEP(assemble, named_scope_brace_after_separator, fix)
{
    // The naming brace may sit on the next line (or after a ':').
    RC_CHECK_TRUE(ASM("ORG &2000\n.routine\n{\n.core LDA #0\n}\nx = routine.core") != 0);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("routine.core")),
                                 value_make_numeric(0x2000)));
}

RC_TEST_STEP(assemble, anonymous_scope_is_private, fix)
{
    RC_CHECK_TRUE(ASM("{ y = 5 }") != 0);
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("y"))));
}

RC_TEST_STEP(assemble, label_does_not_own_following_statement, fix)
{
    // A label hands control straight back: a '}' may sit on the same line right after it (the label
    // no longer swallows what follows), and the inner label still binds to the current pc.
    RC_CHECK_TRUE(ASM("ORG &2000 : .r { .e }") != 0);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("r.e")), value_make_numeric(0x2000)));
}

RC_TEST_STEP(assemble, errors, fix)
{
    RC_CHECK_TRUE(ERR("a.b = 1")            == error_type_invalid_assignment);
    RC_CHECK_TRUE(ERR("}")                  == error_type_unexpected_close_brace);
    RC_CHECK_TRUE(ERR("LDA")                == error_type_missing_operand);
    RC_CHECK_TRUE(ERR("LDA #1 LDA #2")      == error_type_expected_separator);
    RC_CHECK_TRUE(ERR("LDA missing")        == error_type_undefined_symbol);
    RC_CHECK_TRUE(ERR("TAX #5")             == error_type_bad_addressing_mode);
    RC_CHECK_TRUE(ERR("{ LDA #0")           == error_type_unclosed_scope);
}

RC_TEST_STEP(assemble, from_file, fix)
{
    // src/test/sample.6502 (copied next to the tests by CMake) holds "LDA #1 : RTS".
    uint32_t passes = assemble_file(&fix->b, RC_STR("sample.6502"), fix->scratch);
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x01, 0x60}, 3));

    RC_CHECK_TRUE((assemble_file(&fix->b, RC_STR("no_such_file.6502"), fix->scratch),
                   first_error(&fix->b)) == error_type_source_load);
}

RC_TEST_STEP(assemble, if_selects_branch, fix)
{
    // The active branch emits; the dead one is parsed for structure but produces nothing.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #0 : IF 1 : LDA #2 : ENDIF : LDA #3"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x02, 0xA9, 0x03}, 6));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #0 : IF 0 : LDA #2 : ENDIF : LDA #3"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x03}, 4));
}

RC_TEST_STEP(assemble, if_elif_else_chain, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF 1 : LDA #1 : ELIF 1 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x01}, 2));   // first true wins
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF 0 : LDA #1 : ELIF 1 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x02}, 2));   // the elif
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF 0 : LDA #1 : ELIF 0 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x03}, 2));   // the else
}

RC_TEST_STEP(assemble, if_does_not_introduce_scope, fix)
{
    // A label set in a live IF branch leaks to the enclosing scope (IF is not a brace).
    uint32_t passes = ASM("ORG &2000 : IF 1 : .here : ENDIF : LDA here");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xAD, 0x00, 0x20}, 3));
}

RC_TEST_STEP(assemble, if_wraps_scope_and_nests, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF 1 : { LDA #5 } : ENDIF"), (uint8_t[]){0xA9, 0x05}, 2));
    RC_CHECK((ASM("IF 0 : { LDA #5 } : ENDIF"), obj(&fix->b).num), ==, 0u);
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF 1 : IF 1 : LDA #7 : ENDIF : ENDIF"), (uint8_t[]){0xA9, 0x07}, 2));
    RC_CHECK((ASM("IF 1 : IF 0 : LDA #7 : ENDIF : ENDIF"), obj(&fix->b).num), ==, 0u);
    RC_CHECK((ASM("IF 0 : IF 1 : LDA #7 : ENDIF : ENDIF"), obj(&fix->b).num), ==, 0u);   // outer dead -> inner dead
}

RC_TEST_STEP(assemble, if_dead_branch_suppresses_value_errors_but_not_syntax, fix)
{
    // A dead branch raises no value/encoding errors...
    RC_CHECK_TRUE(ERR("IF 0 : LDA undefined_thing : ENDIF") == error_type_none);
    RC_CHECK_TRUE(ERR("IF 0 : TAX #5 : ENDIF") == error_type_none);
    // ...but a genuine parse error still aborts, even when dead.
    RC_CHECK_TRUE(ERR("IF 0 : LDA #1 #2 : ENDIF") == error_type_expected_separator);
}

RC_TEST_STEP(assemble, if_unknown_condition_defers, fix)
{
    // A forward-referenced condition is undecidable on the first pass: both branches stay inactive,
    // and a later pass takes the right one once it resolves.
    uint32_t passes = ASM("IF cond : LDA #1 : ENDIF\ncond = 1");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, if_branch_flip_clears_label, fix)
{
    // `gate` is 1 on the pass after `.other` first appears, then 0 once defined(other) sees it - so
    // `.x` is bound on one pass and must be CLEARED when its branch goes inactive. The remove-on-
    // inactive rule leaves it undefined; a stale binding would linger.
    RC_CHECK_TRUE(ASM("IF gate : .x : ENDIF\ngate = 1 - defined(other)\n.other") != 0);
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("x"))));
}

RC_TEST_STEP(assemble, if_framing_errors, fix)
{
    RC_CHECK_TRUE(ERR("IF 1 : LDA #1")      == error_type_unclosed_if);   // no ENDIF
    RC_CHECK_TRUE(ERR("{ IF 1 : LDA #1 }")  == error_type_unclosed_if);   // '}' before ENDIF
    RC_CHECK_TRUE(ERR("ENDIF")              == error_type_unexpected_endif);
    RC_CHECK_TRUE(ERR("ELIF 1 : ENDIF")     == error_type_unexpected_elif);   // each closer its own error
    RC_CHECK_TRUE(ERR("ELSE")               == error_type_unexpected_else);
    RC_CHECK_TRUE(ERR("IF 1 : ELIF 1 : ELIF 2 : ENDIF") == error_type_none);    // ELIF chain is fine
    RC_CHECK_TRUE(ERR("IF 1 : ELSE : ELIF 1 : ENDIF") == error_type_unexpected_elif);   // ELIF after ELSE
    RC_CHECK_TRUE(ERR("IF 1 : ELSE : ELSE : ENDIF") == error_type_unexpected_else);     // second ELSE
    RC_CHECK_TRUE(ERR("IF 0 : ELSE LDA #1 : ENDIF") == error_type_expected_separator);   // ELSE needs a separator
}

// The next three exercise an IF condition that depends on a forward reference whose own size depends
// on the layout the condition controls - a fixed-point search over passes.

RC_TEST_STEP(assemble, if_forward_ref_condition_settles, fix)
{
    // `LDA fwdlabel` starts absolute (fwdlabel unknown), so the first guess lands fwdlabel at 6; once it
    // shrinks to zero-page, fwdlabel settles at 5, making `fwdlabel = 6` false - the block is skipped.
    uint32_t passes = ASM("LDA #1 : IF fwdlabel = 6 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x01, 0xEA, 0xA5, 0x05, 0x60}, 6));
}

RC_TEST_STEP(assemble, if_forward_ref_both_fixed_points_valid, fix)
{
    // Both "taken" (fwdlabel=10) and "skipped" (fwdlabel=5) are self-consistent fixed points. We
    // converge to whichever our first guess lands on: an unresolved operand resolves to zero-page
    // optimistically (smallest), so fwdlabel starts at 5, `5 > 5` is false, and we settle on skipped.
    uint32_t passes = ASM("LDA #1 : IF fwdlabel > 5 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x01, 0xEA, 0xA5, 0x05, 0x60}, 6));
}

RC_TEST_STEP(assemble, if_forward_ref_contradiction_does_not_converge, fix)
{
    // `fwdlabel = 5` is a contradiction: skipping the block puts fwdlabel at 5 (so the condition is
    // true, contradicting the skip); taking it puts fwdlabel at 10 (so the condition is false). The
    // layout flips between the two forever and never settles.
    RC_CHECK_TRUE(ERR("LDA #1 : IF fwdlabel = 5 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS")
                  == error_type_no_convergence);
}

RC_TEST_STEP(assemble, nested_if_forward_ref, fix)
{
    // Both an outer and an inner IF test the forward label `end`. The fixed code after the conditional
    // block (LDX/LDY) keeps `end` high: pass 1 emits nothing in the stuck branches, leaving end at 6,
    // which already clears both thresholds - so on the next pass both NOPs come in and end settles at 8.
    uint32_t passes = ASM("LDA #0 : IF end >= 4 : NOP : IF end >= 6 : NOP : ENDIF : ENDIF : LDX #1 : LDY #2 : .end : RTS");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x00, 0xEA, 0xEA, 0xA2, 0x01, 0xA0, 0x02, 0x60}, 9));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("end")), value_make_numeric(8)));
}

RC_TEST_STEP(assemble, org_from_forward_ref_ends_at_address, fix)
{
    // The "make this block end at address X" idiom: ORG is computed from a label defined AFTER it, so
    // the 8-byte block sits at &0FF8..&0FFF and progend lands exactly on &1000. ORG is unresolved on
    // the first pass (progstart/progend unknown) and settles once they do; JMP progstart then carries
    // the relocated address. (Code itself still fills the output from index 0.)
    uint32_t passes = ASM("ORG &1000 - (progend - progstart) : .progstart LDA #&41 : JSR &FFEE : JMP progstart : .progend");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x41, 0x20, 0xEE, 0xFF, 0x4C, 0xF8, 0x0F}, 8));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("progstart")), value_make_numeric(0x0FF8)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("progend")), value_make_numeric(0x1000)));
}

RC_TEST_STEP(assemble, for_repeats_body, fix)
{
    // FOR runs its body once per element, emitting a fresh copy each time.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("FOR i = 0..2 : NOP : NEXT"), (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
    // An exclusive range is the same minus its endpoint.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("FOR i = 0..<3 : NOP : NEXT"), (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
}

RC_TEST_STEP(assemble, for_binds_loop_variable, fix)
{
    // The loop variable holds the current element, visible to the body. 0..3 is inclusive (4 elements).
    RC_CHECK_TRUE(code_is(&fix->b, ASM("FOR i = 0..3 : LDA #i : NEXT"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01, 0xA9, 0x02, 0xA9, 0x03}, 8));
    // A list literal drives the loop just as a range does.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("FOR x = {10, 20, 30} : LDA #x : NEXT"),
                          (uint8_t[]){0xA9, 0x0A, 0xA9, 0x14, 0xA9, 0x1E}, 6));
}

RC_TEST_STEP(assemble, for_iteration_has_its_own_scope, fix)
{
    // A label in the body is redefined every iteration; each iteration's private scope keeps that from
    // being a duplicate.
    uint32_t passes = ASM("FOR i = 0..2 : .lbl NOP : NEXT");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
}

RC_TEST_STEP(assemble, for_nests, fix)
{
    // Two iterations of two iterations: four bodies.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("FOR i = 0..1 : FOR j = 0..1 : NOP : NEXT : NEXT"),
                          (uint8_t[]){0xEA, 0xEA, 0xEA, 0xEA}, 4));
}

RC_TEST_STEP(assemble, for_empty_sequence_runs_zero_times, fix)
{
    // An empty sequence is legal and assembles the body zero times (no error): the surrounding code is
    // emitted as if the FOR were absent. Both the empty list literal and a non-ascending exclusive range.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #0 : FOR i = {} : NOP : NEXT : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #0 : FOR i = 5..<5 : NOP : NEXT : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
}

RC_TEST_STEP(assemble, for_forward_ref_count_defers, fix)
{
    // When the count depends on a forward reference, the FOR runs zero times on the first pass and
    // re-expands once the bound resolves.
    uint32_t passes = ASM("FOR i = 0..n : NOP : NEXT\nn = 2");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, for_framing_and_sequence_errors, fix)
{
    RC_CHECK_TRUE(ERR("FOR i = 0..2 : NOP") == error_type_unclosed_for);   // no NEXT
    RC_CHECK_TRUE(ERR("NEXT")               == error_type_unexpected_next);
    RC_CHECK_TRUE(ERR("FOR i = 5 : NOP : NEXT")   == error_type_not_iterable);   // a scalar is not a sequence
    RC_CHECK_TRUE(ERR("FOR i = 5.. : NOP : NEXT") == error_type_not_iterable);  // an unbounded range cannot be counted
}

RC_TEST_STEP(assemble, equb_emits_bytes, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB 1, 2, 3"), (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB &FF"),     (uint8_t[]){0xFF}, 1));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB 255"),     (uint8_t[]){0xFF}, 1));
}

RC_TEST_STEP(assemble, equb_allows_signed_bytes, fix)
{
    // Negative numbers are allowed in -255..255; we emit the low byte.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB -1"),   (uint8_t[]){0xFF}, 1));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB -255"), (uint8_t[]){0x01}, 1));
    RC_CHECK_TRUE(ERR("EQUB 256")  == error_type_value_out_of_range);
    RC_CHECK_TRUE(ERR("EQUB -256") == error_type_value_out_of_range);
}

RC_TEST_STEP(assemble, equs_and_equb_are_aliases, fix)
{
    // EQUS spells out a string char-by-char...
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUS \"ABC\""), (uint8_t[]){0x41, 0x42, 0x43}, 3));
    // ...but takes numbers too, and EQUB takes strings - they are the same directive.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUS 65, 66"),  (uint8_t[]){0x41, 0x42}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB \"Hi\", 0"), (uint8_t[]){0x48, 0x69, 0x00}, 3));
}

RC_TEST_STEP(assemble, equb_ranges_and_lists, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB 1..3"),  (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB 0..<3"), (uint8_t[]){0x00, 0x01, 0x02}, 3));
    // A list is flattened (nested lists and ranges descend); a string element stays whole.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB {1, 2, 3}"),       (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB {1, {2, 3}, 4}"),  (uint8_t[]){0x01, 0x02, 0x03, 0x04}, 4));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB {1, \"Hi\", 2}"),  (uint8_t[]){0x01, 0x48, 0x69, 0x02}, 4));
}

RC_TEST_STEP(assemble, equb_advances_pc, fix)
{
    // The three bytes move pc, so a label after them sees the advanced address.
    RC_CHECK_TRUE(ASM("ORG &2000 : EQUB 1, 2, 3 : .here") != 0);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")), value_make_numeric(0x2003)));
}

RC_TEST_STEP(assemble, equb_forward_reference, fix)
{
    // A forward reference is one byte (a placeholder until it resolves): end - start = 1.
    uint32_t passes = ASM(".start EQUB end - start : .end");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0x01}, 1));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, equb_dead_branch_emits_nothing, fix)
{
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #0 : IF 0 : EQUB 1, 2, 3 : ENDIF : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
}

RC_TEST_STEP(assemble, value_errors_promote_to_specific_codes, fix)
{
    // A value error in an operand keeps its specific cause rather than collapsing to operand_not_numeric,
    // wherever a number is reduced: instruction operand, EQUB, an assignment, a FOR sequence.
    RC_CHECK_TRUE(ERR("LDA #1/0")                 == error_type_divide_by_zero);
    RC_CHECK_TRUE(ERR("EQUB 1/0")                 == error_type_divide_by_zero);
    RC_CHECK_TRUE(ERR("val = 1/0")                == error_type_divide_by_zero);
    RC_CHECK_TRUE(ERR("FOR i = 1/0 : NOP : NEXT") == error_type_divide_by_zero);
    // A genuinely non-numeric operand (a string, not an error value) is still operand_not_numeric.
    RC_CHECK_TRUE(ERR("LDA #\"hi\"") == error_type_operand_not_numeric);
}

RC_TEST_STEP(assemble, errors_accumulate, fix)
{
    // Several independent semantic errors are all reported (not just the first), in source order.
    uint32_t passes = ASM("EQUB 256 : EQUB 257 : TAX #5");
    RC_CHECK_TRUE(passes == 0);
    RC_CHECK(fix->b.diagnostics.num, ==, 3u);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->b.diagnostics.view, 0).code == error_type_value_out_of_range);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->b.diagnostics.view, 1).code == error_type_value_out_of_range);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->b.diagnostics.view, 2).code == error_type_bad_addressing_mode);
    // A syntax (fatal) error, by contrast, aborts the whole assemble with just itself.
    RC_CHECK((ASM("} ENDIF NEXT"), fix->b.diagnostics.num), ==, 1u);
}

RC_TEST_STEP(assemble, duplicate_symbol_signposts_original, fix)
{
    // A duplicate raises the error at the redefinition AND a companion note pointing back at the first
    // binding (both severity-0 errors, so both always show). The note's location precedes the error's.
    RC_CHECK_TRUE(ASM(".dup NOP : .dup RTS") == 0);
    RC_CHECK(fix->b.diagnostics.num, ==, 2u);
    diagnostic err  = rc_view_diagnostic_get(fix->b.diagnostics.view, 0);
    diagnostic note = rc_view_diagnostic_get(fix->b.diagnostics.view, 1);
    RC_CHECK_TRUE(err.code == error_type_duplicate_symbol);
    RC_CHECK_TRUE(note.code == error_type_original_definition);
    RC_CHECK_TRUE(note.at.pos < err.at.pos);   // the original sits earlier in the source than the redefinition
    // Same for assignments.
    RC_CHECK_TRUE(ERR("twice = 1 : twice = 2") == error_type_duplicate_symbol);
    RC_CHECK(fix->b.diagnostics.num, ==, 2u);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->b.diagnostics.view, 1).code == error_type_original_definition);
}

RC_TEST_STEP(assemble, failure_clears_outputs, fix)
{
    // A failed assemble hands back no object code and no symbols - only the diagnostics remain.
    RC_CHECK_TRUE(ASM("ORG &2000 : .lbl LDA #0 : TAX #5") == 0);   // TAX #5 has no encoding
    RC_CHECK(obj(&fix->b).num, ==, 0u);
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("lbl"))));
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_bad_addressing_mode));
}

RC_TEST_STEP(assemble, warnings_do_not_fail_assembly, fix)
{
    // JMP (&xxFF) is legal but trips the NMOS vector-fetch page-wrap bug: a warning, not an error, so the
    // assemble still succeeds and emits the three bytes - the warning just rides along in the diagnostics.
    uint32_t passes = ASM("JMP (&12FF)");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0x6C, 0xFF, 0x12}, 3));
    RC_CHECK_TRUE(first_error(&fix->b) == error_type_none);   // no error-severity diagnostic
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_jmp_indirect_page_cross));
    // A vector that does not straddle a page boundary is silent.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("JMP (&1234)"), (uint8_t[]){0x6C, 0x34, 0x12}, 3));
    RC_CHECK(fix->b.diagnostics.num, ==, 0u);
}

RC_TEST_STEP(assemble, named_constants, fix)
{
    // A pure constant flows through the operand path like any number...
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #TRUE"),  (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("LDA #FALSE"), (uint8_t[]){0xA9, 0x00}, 2));
    // ...and serves as an IF condition.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF TRUE : LDA #1 : ELSE : LDA #2 : ENDIF"),  (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("IF FALSE : LDA #1 : ELSE : LDA #2 : ENDIF"), (uint8_t[]){0xA9, 0x02}, 2));
    // The constants are reserved: you cannot redefine one (it would otherwise bind a symbol shadowed by the
    // constant in every expression).
    RC_CHECK_TRUE(ERR("PI = 5") == error_type_reserved_constant);
    RC_CHECK_TRUE(ERR("TRUE = 1") == error_type_reserved_constant);
    RC_CHECK_TRUE(ERR("FALSE = 0") == error_type_reserved_constant);
}

RC_TEST_STEP(assemble, pc_constant, fix)
{
    // '*' (and its BBC Micro alias P%) is the current PC. It is evaluated before the instruction emits, so
    // JMP * is the classic jump-to-self.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("ORG &2000 : JMP *"),  (uint8_t[]){0x4C, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(&fix->b, ASM("ORG &2000 : JMP P%"), (uint8_t[]){0x4C, 0x00, 0x20}, 3));
    // Mid-program it reads the live PC: after LDA #0 (two bytes), * is org+2.
    uint32_t passes = ASM("ORG &2000 : LDA #0 : here = * : RTS");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA9, 0x00, 0x60}, 3));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("here")), value_make_numeric(0x2002)));
    // A comma list evaluates one element at a time, so the PC advances between them (org 0 here: 0, 1, 2)...
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB *, *, *"), (uint8_t[]){0x00, 0x01, 0x02}, 3));
    // ...but a list literal is one value computed at one instant, so every * is the same PC.
    RC_CHECK_TRUE(code_is(&fix->b, ASM("EQUB {*, *, *}"), (uint8_t[]){0x00, 0x00, 0x00}, 3));
}

// INCLUDE tests give the top source an explicit slash-free name: ASM names a source after its own text,
// which would drop the include's own slashes into the cache key and wreck the relative-path peel. Each
// step gets a fresh baron, so the one name "top" never collides.
#define INC(src) assemble_string(&fix->b, RC_STR("top"), RC_STR(src), fix->scratch)

RC_TEST_STEP(assemble, include_splices_file, fix)
{
    // INCLUDE pulls the file in textually: its instruction emits right here, and its label binds in THIS
    // scope (no scope of its own).
    uint32_t passes = INC("include \"inc_child.6502\"");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA2, 0x02}, 2));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("child")), value_make_numeric(0)));
}

RC_TEST_STEP(assemble, include_resolves_relative_to_includer, fix)
{
    // sub/mid.6502 does its own INCLUDE "leaf.6502" - resolved against sub/, not the top file's directory.
    uint32_t passes = INC("include \"sub/mid.6502\"");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA0, 0x03, 0xEA}, 3));
}

RC_TEST_STEP(assemble, include_joins_the_multi_pass, fix)
{
    // `target` sits after the include, so its address depends on the included file's two bytes - it only
    // settles once the include is part of the pass loop. JMP is fixed-width, so this converges cleanly.
    uint32_t passes = INC("jmp target : include \"inc_fwd_child.6502\" : .target rts");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0x4C, 0x05, 0x00, 0xEA, 0xEA, 0x60}, 6));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->b.scopes, 0, RC_STR("target")), value_make_numeric(5)));
}

RC_TEST_STEP(assemble, include_reports_included_from_frame, fix)
{
    // The included file has an out-of-range immediate. That error surfaces, followed by a frame pointing
    // back at the INCLUDE - so a failure inside an include names both the real site and how we reached it.
    RC_CHECK(INC("include \"inc_bad_child.6502\""), ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_value_out_of_range));
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_included_from));
}

RC_TEST_STEP(assemble, include_missing_file_is_an_error, fix)
{
    RC_CHECK(INC("include \"no_such_baron_file.6502\""), ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_source_load));
}

RC_TEST_STEP(assemble, include_forward_declared_filename, fix)
{
    // The filename is a symbol only bound AFTER the INCLUDE: it defers on pass 1 (like a forward address)
    // and the file loads once the name settles.
    uint32_t passes = INC("include fname : fname = \"inc_child.6502\"");
    RC_CHECK_TRUE(code_is(&fix->b, passes, (uint8_t[]){0xA2, 0x02}, 2));
    // A name that never binds (nothing defines it) is an undefined symbol at the INCLUDE, not a hang.
    RC_CHECK(assemble_string(&fix->b, RC_STR("top2"), RC_STR("include missing_name"), fix->scratch), ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_undefined_symbol));
}

RC_TEST_STEP(assemble, include_in_dead_branch_is_skipped, fix)
{
    // A dead INCLUDE never even goes looking for its file, so a missing include under IF 0 assembles clean.
    uint32_t passes = INC("if 0 : include \"no_such_baron_file.6502\" : endif");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK(fix->b.diagnostics.num, ==, 0u);
}

RC_TEST_STEP(assemble, include_cycle_is_caught, fix)
{
    // inc_cycle.6502 includes itself; the depth cap stops the recursion rather than blowing the C stack.
    uint32_t passes = assemble_file(&fix->b, RC_STR("inc_cycle.6502"), fix->scratch);
    RC_CHECK(passes, ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->b, error_type_include_too_deep));
}

#endif // BARON_TESTS
