#include "assemble_internal.h"   // the parsing vocabulary (and assemble.h via it)

#include "opcodes.h"
#include "lexer.h"
#include "expression.h"
#include "baron.h"               // the owner type the tests assemble into
#include "richc/macros.h"


#define ASSEMBLE_MAX_PASSES 100u


// Mutual recursion: a label or a scope reopens the statement loop, and the loop reaches the
// handlers that may do so. The directive handlers are referenced by the statement table.
static parse_result handle_org(parse_input in, scopes *s, overlay *o, rc_arena scratch);
static parse_result handle_label(parse_input in, scopes *s, overlay *o, rc_arena scratch);
static parse_result handle_open_brace(parse_input in, scopes *s, overlay *o, rc_arena scratch);
static parse_result parse_block(parse_input in, scopes *s, overlay *o, rc_arena scratch, bool is_file);
static parse_result parse_one_statement(parse_input in, scopes *s, overlay *o, rc_arena scratch);


// operand_value is shared with opcodes.c (both declared in assemble.h); it needs no token
// table, so it sits on its own. Pure: it reads v and returns the reduction.
operand operand_value(value v, bool final_pass, uint32_t at)
{
    if (value_is_numeric(v)) {
        return (operand) {
            .known = true,
            .addr = (int64_t) v.numeric
        };
    }
    if (value_is_error(v) && v.error == value_error_unknown_symbol) {
        if (final_pass) {
            return (operand) {
                .error = assemble_error_undefined_symbol,
                .error_at = at
            };
        }
        return (operand) { .unresolved = true };   // a forward reference; settles on a later pass
    }
    
    return (operand) {
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

    {RC_STR("org"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_org}}},
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

// A copy of in advanced to a new cursor, for the "consume and recurse" step.
static parse_input at(parse_input in, uint32_t cursor)
{
    in.cursor = cursor;
    return in;
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

static parse_result handle_org(parse_input in, scopes *s, overlay *o, rc_arena scratch)
{
    expr_result e = expression_parse(in.source, in.cursor, s, in.scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    operand op = operand_value(e.value, in.final_pass, in.cursor);
    if (op.error != assemble_error_none) return parse_fail(op.error, op.error_at);
    if (op.known) {
        overlay_org(o, (uint32_t)(op.addr & 0xFFFF));
    }
    // An unknown ORG leaves pc as-is this pass; op.unresolved forces another pass.
    parse_result r = require_separator(in.source, e.next);
    r.unresolved = op.unresolved;
    return r;
}

static parse_result handle_label(parse_input in, scopes *s, overlay *o, rc_arena scratch)
{
    // The label name. A name spelled exactly like a mnemonic would lex as that opcode (the same
    // limitation an assignment target has at statement start) - acceptable, and not worth a
    // private name table.
    lexer_result nm = lexer_next(in.source, in.cursor, statement_tokens);
    if (nm.token.type != lexeme_type_identifier || is_dotted(nm.token.identifier.name)) {
        return parse_fail(assemble_error_expected_label_name, in.cursor);
    }
    rc_str name = nm.token.identifier.name;

    // Bind name = current pc in the current scope; a moved label drives another pass.
    parse_result r = { .next = nm.next };
    r.changed = scopes_set_symbol(s, in.scope, name, value_make_numeric((double)o->pc));

    // What follows decides the label's shape. A '{' names a scope - and may sit on the next
    // line, so a single separator before it is allowed. A bare separator leaves a stand-alone
    // label; anything else shares the line as its own statement.
    lexer_result nb = lexer_next(in.source, r.next, statement_tokens);
    if (nb.token.type == lexeme_type_terminator) {
        lexer_result after = lexer_next(in.source, nb.next, statement_tokens);
        if (!is_open_brace(after.token)) {
            return r;                          // stand-alone label - leave the terminator for the loop
        }
        nb = after;                            // .label <separator> { ... } still names the scope
    }

    if (is_open_brace(nb.token)) {
        uint32_t    child = scopes_get_or_make_child(s, in.scope, name);
        parse_input inner = at(in, nb.next);
        inner.scope = child;
        return fold(r, parse_block(inner, s, o, scratch, false));
    }
    return fold(r, parse_one_statement(at(in, r.next), s, o, scratch));   // shares the line
}

static parse_result handle_open_brace(parse_input in, scopes *s, overlay *o, rc_arena scratch)
{
    // in.cursor is just past the '{'; its offset gives the anonymous scope a stable per-pass
    // identity, so re-walking it on a later pass keeps the same bindings.
    parse_input inner = in;
    inner.scope = scopes_get_or_make_child_at(s, in.scope, in.cursor - 1);
    return parse_block(inner, s, o, scratch, false);
}


// ---- the assignment statement ----

// '=' is its own tiny table, lexed only after an identifier at statement start.
static const token assign_token_entries[] = {
    {RC_STR("="), {.type = lexeme_type_assign}},
};
static const token_table assign_tokens = RC_VIEW(assign_token_entries);

// The identifier and the cursor past it are in `in` (cursor) and `name`; an assignment defines
// a symbol but emits nothing, so it takes no overlay.
static parse_result handle_assignment(parse_input in, rc_str name, scopes *s, rc_arena scratch)
{
    if (is_dotted(name)) {
        return parse_fail(assemble_error_invalid_assignment, in.cursor);
    }
    lexer_result eq = lexer_next(in.source, in.cursor, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return parse_fail(assemble_error_expected_assign, in.cursor);
    }

    expr_result e = expression_parse(in.source, eq.next, s, in.scope, &scratch);
    if (e.error != expr_error_none) return parse_fail(assemble_error_expression, e.error_at);

    parse_result r = { .next = e.next };
    if (value_is_error(e.value) && e.value.error == value_error_unknown_symbol) {
        if (in.final_pass) return parse_fail(assemble_error_undefined_symbol, eq.next);
        r.unresolved = true;   // a forward reference in the value; settles on a later pass
    }
    r.changed = scopes_set_symbol(s, in.scope, name, e.value);
    return fold(r, require_separator(in.source, e.next));
}


// ---- the statement loop ----

static parse_result parse_one_statement(parse_input in, scopes *s, overlay *o, rc_arena scratch)
{
    lexer_result lr = lexer_next(in.source, in.cursor, statement_tokens);
    switch (lr.token.type) {
        case lexeme_type_opcode:
            return opcode_parse((mnemonic)lr.token.opcode.id, at(in, lr.next), s, o, scratch);
        case lexeme_type_keyword:
            return lr.token.keyword.handle(at(in, lr.next), s, o, scratch);   // ORG, '.', or '{'
        case lexeme_type_identifier:
            return handle_assignment(at(in, lr.next), lr.token.identifier.name, s, scratch);
        default:
            return parse_fail(assemble_error_unexpected_token, in.cursor);
    }
}

static parse_result parse_block(parse_input in, scopes *s, overlay *o, rc_arena scratch, bool is_file)
{
    parse_result acc = { .next = in.cursor };
    while (true) {
        lexer_result lr = lexer_next(in.source, acc.next, statement_tokens);

        if (lr.token.type == lexeme_type_terminator) {
            acc.next = lr.next;
            if (lexer_at_end(in.source, acc.next)) {
                if (!is_file) { acc.error = assemble_error_unclosed_scope; acc.error_at = acc.next; }
                return acc;
            }
            continue;                      // blank statement
        }
        if (lr.token.type == lexeme_type_close_brace) {
            acc.next = lr.next;
            if (is_file) { acc.error = assemble_error_unexpected_close_brace; acc.error_at = acc.next; }
            return acc;
        }
        acc = fold(acc, parse_one_statement(at(in, acc.next), s, o, scratch));
        if (acc.error != assemble_error_none) {
            return acc;
        }
    }
}


// ---- the multi-pass driver ----

static parse_result run_pass(scopes *s, overlay *o, rc_str source, rc_arena scratch, bool final_pass)
{
    overlay_reset(o);
    parse_input in = {
        .source = source,
        .cursor = 0,
        .scope = 0,
        .final_pass = final_pass
    };
    return parse_block(in, s, o, scratch, true);
}

assemble_result assemble(scopes *s, overlay *o, rc_str source, rc_arena scratch)
{
    RC_ASSERT(s != NULL && o != NULL);

    for (uint32_t pass = 1; pass <= ASSEMBLE_MAX_PASSES; pass++) {
        parse_result r = run_pass(s, o, source, scratch, false);
        if (r.error != assemble_error_none) {
            return (assemble_result) {
                .passes = pass,
                .error = r.error,
                .error_at = r.error_at
            };
        }
        if (!r.unresolved && !r.changed) {
            // Settled. One more pass with checks armed, to surface any deferred range errors.
            parse_result fin = run_pass(s, o, source, scratch, true);
            if (fin.error != assemble_error_none) {
                return (assemble_result) {
                    .passes = pass + 1,
                    .error = fin.error,
                    .error_at = fin.error_at
                };
            }
            return (assemble_result) {
                .code = o->code.view,
                .passes = pass + 1
            };
        }
    }

    // Did not settle within the cap. A final diagnostic pass names a concrete cause (an
    // undefined symbol) where there is one; otherwise it is genuine oscillation.
    parse_result diag = run_pass(s, o, source, scratch, true);
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
        case assemble_error_undefined_symbol:       return RC_STR("undefined_symbol");
        case assemble_error_expression:             return RC_STR("expression");
        case assemble_error_no_convergence:         return RC_STR("no_convergence");
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

#define RESULT(src) assemble(&fix->b.scopes, &fix->b.overlay, RC_STR(src), fix->scratch)

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
    RC_CHECK_TRUE(code_is(RESULT("BEQ skip : NOP : .skip"), (uint8_t[]){0xF0, 0x01, 0xEA}, 3));
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

#endif // BARON_TESTS
