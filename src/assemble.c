#include "assemble_internal.h"   // the parsing vocabulary (and assemble.h via it)

#include "opcodes.h"
#include "lexer.h"
#include "expression.h"
#include "baron.h"               // the owner type the tests assemble into
#include "richc/macros.h"


#define ASSEMBLE_MAX_PASSES 100u


// Mutual recursion: a label or a scope reopens the statement loop, and the loop reaches the
// handlers that may do so. The directive handlers are referenced by the statement table.
// All the parse functions take the same head: the baron (its scopes / overlays / source files,
// and the current overlay), then the current source and scope index, the cursor, and final_pass,
// then scratch by value. Each fetches its source rc_str from b->source_files at the targ.
static parse_result handle_org(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result handle_skip(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result handle_skipto(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result handle_align(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result handle_label(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result handle_open_brace(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result parse_block(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result parse_scope(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);
static parse_result parse_one_statement(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch);


// parse_argument_make is shared with opcodes.c (both declared in assemble.h); it needs no token
// table, so it sits on its own. Pure: it reads v and returns the reduction.
parse_argument parse_argument_make(value v, bool final_pass, uint32_t at)
{
    if (value_is_numeric(v)) {
        return (parse_argument) {
            .type = parse_argument_type_known,
            .value = (int64_t) v.numeric
        };
    }
    if (value_is_error(v) && v.error == value_error_unknown_symbol) {
        if (final_pass) {
            return (parse_argument) {
                .type = parse_argument_type_error,
                .error = assemble_error_undefined_symbol,
                .error_at = at
            };
        }
        return (parse_argument) { .type = parse_argument_type_unresolved };   // a forward reference; settles later
    }

    return (parse_argument) {
        .type = parse_argument_type_error,
        .error = assemble_error_operand_not_numeric,
        .error_at = at
    };
}


// ---- the statement token table and the framing it drives ----

// Pre-combined: every mnemonic (its own lexeme type, carrying the id) plus the statement
// directives (ORG, the '.' label introducer, the '{' scope opener) and the '}' closer. '#' and
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

    {RC_STR("org"),    {.type = lexeme_type_keyword, .keyword = {.handle = handle_org}}},
    {RC_STR("skip"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_skip}}},
    {RC_STR("skipto"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_skipto}}},
    {RC_STR("align"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_align}}},
    {RC_STR("."),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_label}}},
    {RC_STR("{"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_open_brace}}},
    {RC_STR("}"),   {.type = lexeme_type_close_brace}},
};
static const token_table statement_tokens = RC_VIEW(statement_token_entries);

// require_separator is shared with opcodes.c (declared in assemble.h); it reads the statement
// table to recognise the '}' that implicitly closes a one-liner, so it lives here with it.
parse_result require_separator(rc_str source, uint32_t cursor)
{
    lexer_result r = lexer_next(source, cursor, statement_tokens);
    if (r.token.type == lexeme_type_terminator) {
        return (parse_result) { .next = r.next };
    }
    if (r.token.type == lexeme_type_close_brace) {
        return (parse_result) { .next = cursor };   // '}' closes the statement; parse_block consumes it
    }
    return parse_fail(assemble_error_expected_separator, cursor);
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

// Fold a sub-parse's outcome into r: take its cursor, OR its flags, adopt its (first) error.
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

static parse_result handle_org(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str      src = source_files_text(&b->source_files, source);
    expr_result e   = expression_parse(src, cursor, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    parse_argument arg = parse_argument_make(e.value, final_pass, cursor);
    if (arg.type == parse_argument_type_error) return parse_fail(arg.error, arg.error_at);
    if (arg.type == parse_argument_type_known) {
        overlays_org(&b->overlays, b->current_overlay, (uint32_t)(arg.value & 0xFFFF));
    }
    // An unknown ORG leaves pc as-is this pass; an unresolved argument forces another pass.
    parse_result r = require_separator(src, e.next);
    r.unresolved = (arg.type == parse_argument_type_unresolved);
    return r;
}

// SKIP n - pad the object code with n zero bytes (advancing pc by n). A negative count would
// rewind the pointer, which we cannot do; the layout-dependent check is deferred to the final pass.
static parse_result handle_skip(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str      src = source_files_text(&b->source_files, source);
    expr_result e   = expression_parse(src, cursor, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    parse_argument arg = parse_argument_make(e.value, final_pass, cursor);
    if (arg.type == parse_argument_type_error) return parse_fail(arg.error, arg.error_at);
    if (arg.type == parse_argument_type_known) {
        if (arg.value < 0) {
            if (final_pass) return parse_fail(assemble_error_skip_backwards, cursor);
        } else {
            overlays_skip(&b->overlays, b->current_overlay, (uint32_t) arg.value);
        }
    }
    parse_result r = require_separator(src, e.next);
    r.unresolved = (arg.type == parse_argument_type_unresolved);   // an unknown count emits nothing this pass; forces another
    return r;
}

// SKIPTO addr - pad with zeroes until pc reaches addr. Being already past addr is an error,
// deferred to the final pass since pc only settles once preceding forward references resolve.
static parse_result handle_skipto(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str      src = source_files_text(&b->source_files, source);
    expr_result e   = expression_parse(src, cursor, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    parse_argument arg = parse_argument_make(e.value, final_pass, cursor);
    if (arg.type == parse_argument_type_error) return parse_fail(arg.error, arg.error_at);
    if (arg.type == parse_argument_type_known) {
        int64_t pc = (int64_t) overlays_pc(&b->overlays, b->current_overlay);
        if (arg.value < pc) {
            if (final_pass) return parse_fail(assemble_error_skip_backwards, cursor);
        } else {
            overlays_skip(&b->overlays, b->current_overlay, (uint32_t) (arg.value - pc));
        }
    }
    parse_result r = require_separator(src, e.next);
    r.unresolved = (arg.type == parse_argument_type_unresolved);
    return r;
}

// ALIGN n - pad with zeroes until pc is a multiple of n. n < 1 is meaningless (and would divide
// by zero), so it is an error; the modulo is only evaluated once we know n is sound.
static parse_result handle_align(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str      src = source_files_text(&b->source_files, source);
    expr_result e   = expression_parse(src, cursor, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    parse_argument arg = parse_argument_make(e.value, final_pass, cursor);
    if (arg.type == parse_argument_type_error) return parse_fail(arg.error, arg.error_at);
    if (arg.type == parse_argument_type_known) {
        if (arg.value < 1) {
            if (final_pass) return parse_fail(assemble_error_bad_alignment, cursor);
        } else {
            uint32_t n   = (uint32_t) arg.value;
            uint32_t rem = overlays_pc(&b->overlays, b->current_overlay) % n;
            if (rem != 0) {
                overlays_skip(&b->overlays, b->current_overlay, n - rem);
            }
        }
    }
    parse_result r = require_separator(src, e.next);
    r.unresolved = (arg.type == parse_argument_type_unresolved);
    return r;
}

static parse_result handle_label(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, source);

    // The label name. A name spelled exactly like a mnemonic would lex as that opcode (the same
    // limitation an assignment target has at statement start) - acceptable, and not worth a
    // private name table.
    lexer_result nm = lexer_next(src, cursor, statement_tokens);
    if (nm.token.type != lexeme_type_identifier || is_dotted(nm.token.identifier.name)) {
        return parse_fail(assemble_error_expected_label_name, cursor);
    }
    rc_str name = nm.token.identifier.name;

    // Bind name = current pc in the current scope; a moved label drives another pass, and a
    // second definition of the same name here (a different source position) is a duplicate.
    parse_result r = {
        .next = nm.next
    };
    source_pos def = {
        .source = source,
        .offset = cursor
    };

    symbol_status st = scopes_set_symbol(&b->scopes, scope, name,
                                         value_make_numeric((double)overlays_pc(&b->overlays, b->current_overlay)), def);

    if (st == symbol_status_duplicate) {
        return parse_fail(assemble_error_duplicate_symbol, cursor);
    }
    r.changed = (st == symbol_status_changed);

    // What follows decides the label's shape. A '{' names a scope - and may sit on the next line,
    // so a single separator before it is allowed. A bare separator leaves a stand-alone label;
    // anything else shares the line as its own statement.
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
        return fold(r, parse_scope(b, source, child, nb.next, final_pass, scratch));
    }
    return fold(r, parse_one_statement(b, source, scope, r.next, final_pass, scratch));   // shares line
}

static parse_result handle_open_brace(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    // The just-passed cursor gives the anonymous scope a stable per-pass identity, so re-walking it
    // on a later pass keeps the same bindings.
    uint32_t child = scopes_get_or_make_child_at(&b->scopes, scope, (source_pos){ .source = source, .offset = cursor });
    return parse_scope(b, source, child, cursor, final_pass, scratch);
}


// ---- the assignment statement ----

// '=' is its own tiny table, lexed only after an identifier at statement start.
static const token assign_token_entries[] = {
    {RC_STR("="), {.type = lexeme_type_assign}},
};
static const token_table assign_tokens = RC_VIEW(assign_token_entries);

// `name` is the identifier and `cursor` sits just past it; an assignment defines a symbol and
// emits nothing.
static parse_result handle_assignment(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_str name, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, source);
    if (is_dotted(name)) {
        return parse_fail(assemble_error_invalid_assignment, cursor);
    }
    lexer_result eq = lexer_next(src, cursor, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return parse_fail(assemble_error_expected_assign, cursor);
    }

    expr_result e = expression_parse(src, eq.next, &b->scopes, scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    // A second definition of the same name here (a different source position) is a duplicate,
    // independent of whether the value has resolved yet, so check it first.
    source_pos def = {
        .source = source,
        .offset = cursor
    };
    symbol_status st  = scopes_set_symbol(&b->scopes, scope, name, e.value, def);
    if (st == symbol_status_duplicate) {
        return parse_fail(assemble_error_duplicate_symbol, cursor);
    }

    parse_result r = {
        .next = e.next
    };
    if (value_is_error(e.value) && e.value.error == value_error_unknown_symbol) {
        if (final_pass) {
            return parse_fail(assemble_error_undefined_symbol, eq.next);
        }
        r.unresolved = true;   // a forward reference in the value; settles on a later pass
    }
    r.changed = (st == symbol_status_changed);
    return fold(r, require_separator(src, e.next));
}


// ---- the statement loop ----

static parse_result parse_one_statement(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str       src = source_files_text(&b->source_files, source);
    lexer_result lr  = lexer_next(src, cursor, statement_tokens);
    switch (lr.token.type) {
        case lexeme_type_opcode:
            return opcode_parse(b, (mnemonic)lr.token.opcode.id, source, scope, lr.next, final_pass, scratch);
        case lexeme_type_keyword:
            return lr.token.keyword.handle(b, source, scope, lr.next, final_pass, scratch);   // ORG, '.', or '{'
        case lexeme_type_identifier:
            return handle_assignment(b, source, scope, lr.next, final_pass, lr.token.identifier.name, scratch);
        default:
            return parse_fail(assemble_error_unexpected_token, cursor);
    }
}

// Parse statements until the block's closer: a '}' or end of input. It stops AT the closer -
// leaving a '}' unconsumed - and treats neither as an error, because which closer is required
// depends on the caller: parse_scope wants the '}', run_pass wants end of input.
static parse_result parse_block(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, source);
    parse_result acc = {
        .next = cursor
    };

    while (acc.error == assemble_error_none) {
        lexer_result lr = lexer_next(src, acc.next, statement_tokens);

        if (lr.token.type == lexeme_type_close_brace) {
            return acc;                    // stop at the '}', leaving it for the caller to close on
        }

        if (lr.token.type == lexeme_type_terminator) {
            if (lexer_at_end(src, lr.next)) {
                acc.next = lr.next;
                return acc;                // stop at end of input
            }
            acc.next = lr.next;            // blank statement
            continue;
        }

        acc = fold(acc, parse_one_statement(b, source, scope, acc.next, final_pass, scratch));
    }
    return acc;
}

// A braced block: parse its statements (entered just past the '{') and require the closing '}';
// reaching end of input first is an unclosed scope.
static parse_result parse_scope(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    parse_result r = parse_block(b, source, scope, cursor, final_pass, scratch);
    if (r.error != assemble_error_none) {
        return r;
    }

    rc_str       src = source_files_text(&b->source_files, source);
    lexer_result lr  = lexer_next(src, r.next, statement_tokens);
    if (lr.token.type == lexeme_type_close_brace) {
        r.next = lr.next;
        return r;
    }

    r.error    = assemble_error_unclosed_scope;
    r.error_at = r.next;
    return r;
}


// A whole source file - the largest parsable item: parse its statements and require they run out
// at end of input. A leftover '}' is a stray close brace. (parse_scope is a braced block within a
// file; parse_block is the shared statement loop both rest on.)
static parse_result parse_file(baron *b, uint32_t source, uint32_t scope, uint32_t cursor, bool final_pass, rc_arena scratch)
{
    parse_result r = parse_block(b, source, scope, cursor, final_pass, scratch);
    if (r.error != assemble_error_none) return r;

    rc_str       src = source_files_text(&b->source_files, source);
    lexer_result lr  = lexer_next(src, r.next, statement_tokens);
    if (lr.token.type == lexeme_type_close_brace) {
        r.error    = assemble_error_unexpected_close_brace;
        r.error_at = lr.next;
    }
    return r;
}


// ---- the multi-pass driver ----

static parse_result run_pass(baron *b, uint32_t source, bool final_pass, rc_arena scratch)
{
    overlays_reset_all(&b->overlays);
    b->current_overlay = overlays_default;   // each pass re-derives the current overlay from the source
    return parse_file(b, source, /*scope*/ 0, /*cursor*/ 0, final_pass, scratch);
}

// The shared core: run passes over the source already cached at index `source` in b, until the
// labels and forward references settle (or non-convergence). assemble_string / assemble_file are
// thin wrappers that establish the source entry, then call this.
static assemble_result run_passes(baron *b, uint32_t source, rc_arena scratch)
{
    for (uint32_t pass = 1; pass <= ASSEMBLE_MAX_PASSES; pass++) {
        parse_result r = run_pass(b, source, false, scratch);
        if (r.error != assemble_error_none) {
            return (assemble_result) {
                .passes = pass,
                .error = r.error,
                .error_at = r.error_at
            };
        }
        if (!r.unresolved && !r.changed) {
            // Settled. One more pass with checks armed, to surface any deferred range errors.
            parse_result fin = run_pass(b, source, true, scratch);
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

    // Did not settle within the cap. A final diagnostic pass names a concrete cause (an
    // undefined symbol) where there is one; otherwise it is genuine oscillation.
    parse_result diag = run_pass(b, source, true, scratch);
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

#endif // BARON_TESTS
