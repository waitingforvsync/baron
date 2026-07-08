#include "assemble_internal.h"   // the parsing vocabulary (and assemble.h via it)

#include "opcodes.h"
#include "lexer.h"
#include "expression.h"
#include "cfg.h"                 // post-convergence zero-page allocation: recover the CFG...
#include "liveness.h"            // ...run liveness over it...
#include "footprint.h"           // ...find each callee's footprint for across-call interference...
#include "zpalloc.h"             // ...and colour the interference graph into the reserved bytes
#include "file_utils.h"          // INCLUDE / INCBIN path resolution
#include "richc/file.h"          // INCBIN: rc_file_size / rc_file_load_binary
#include "baron.h"               // the owner type the tests assemble into
#include "richc/macros.h"


#define ASSEMBLE_MAX_PASSES 100u
#define ASSEMBLE_MAX_INCLUDE_DEPTH 64u   // a runaway / cyclic INCLUDE is caught here before the C stack gives out
#define ASSEMBLE_MAX_MACRO_DEPTH 64u     // a runaway macro expansion (a missing recursion base case) is caught here


// Mutual recursion: a label or a scope reopens the statement loop, and the loop reaches the
// handlers that may do so. The directive handlers are referenced by the statement table.
// All the parse functions take the same head: the baron (its scopes / sections / source files, and
// the current section), then the cursor `at` (source file index plus offset), the scope index, and
// the parse flags, then scratch by value. Each fetches its source rc_str from b->source_files at the top.
static parse_result handle_skip(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_skipto(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_align(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_section(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_zpreserve(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_zpauto1(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_zpauto2(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_unreachable(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_cancall(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_canjump(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_equb(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_equw(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_equd(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_label(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_local_label(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_open_brace(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_if(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_for(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_include(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_incbin(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_macro(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
static parse_result handle_macro_invocation(baron *b, cursor at, uint32_t scope, parse_flags flags, uint32_t macro_index, rc_arena scratch);
static parse_result handle_function(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch);
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
    closer_endmacro,
    closer_endsection,
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
    {RC_STR(".@"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_local_label}}},
    {RC_STR("{"),      {.type = lexeme_type_keyword, .keyword = {.handle = handle_open_brace}}},
    {RC_STR("skip"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_skip}}},
    {RC_STR("skipto"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_skipto}}},
    {RC_STR("align"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_align}}},
    {RC_STR("section"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_section}}},
    {RC_STR("zpreserve"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_zpreserve}}},
    {RC_STR("zpauto1"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_zpauto1}}},   // 1-byte ZP variable
    {RC_STR("zpauto2"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_zpauto2}}},   // 2-byte ZP variable
    {RC_STR("unreachable"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_unreachable}}}, // dead fall-through
    {RC_STR("cancall"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_cancall}}},   // a JSR's real targets
    {RC_STR("canjump"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_canjump}}},   // a computed JMP's targets
    {RC_STR("equb"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equb}}},
    {RC_STR("equs"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equb}}},   // EQUS is an alias of EQUB
    {RC_STR("equw"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equw}}},   // 16-bit words
    {RC_STR("equd"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equd}}},   // 32-bit words
    {RC_STR("if"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_if}}},
    {RC_STR("for"),    {.type = lexeme_type_keyword, .keyword = {.handle = handle_for}}},
    {RC_STR("include"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_include}}},
    {RC_STR("incbin"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_incbin}}},
    {RC_STR("macro"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_macro}}},
    {RC_STR("function"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_function}}},
    // The pure expression constants are reserved at statement start too, so `pi = 5` is rejected rather than
    // quietly binding a shadowed symbol. Three near-identical rows, but it is only three tokens.
    {RC_STR("true"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR("false"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR("pi"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR("elif"),   {.type = lexeme_type_closer, .closer = {closer_elif,  error_type_unexpected_elif}}},
    {RC_STR("else"),   {.type = lexeme_type_closer, .closer = {closer_else,  error_type_unexpected_else}}},
    {RC_STR("endif"),  {.type = lexeme_type_closer, .closer = {closer_endif, error_type_unexpected_endif}}},
    {RC_STR("next"),    {.type = lexeme_type_closer, .closer = {closer_next,     error_type_unexpected_next}}},
    {RC_STR("endmacro"),{.type = lexeme_type_closer, .closer = {closer_endmacro, error_type_unexpected_endmacro}}},
    {RC_STR("endsection"),{.type = lexeme_type_closer, .closer = {closer_endsection, error_type_unexpected_endsection}}},
    {RC_STR("}"),       {.type = lexeme_type_closer, .closer = {closer_brace,    error_type_unexpected_close_brace}}},
};

// The STATIC base: mnemonics, directives and closers. Macro-name tokens are appended to a per-pass COPY of
// this (macros.statement_tokens), so a name defined earlier this pass is recognised as a call; statement_tokens(b)
// returns that live table. handle_macro lexes the macro NAME from the base instead, so an already-defined name
// still reads as a plain identifier there (and overloads reuse one name entry).
static const token_table base_statement_tokens = RC_VIEW(statement_token_entries);

// The statement table THIS pass - the base plus one token per macro name seen so far. Every statement-start
// lex goes through it, so a label / for-var / symbol spelled like a macro reads as a call (and errors), the
// same acceptable collision a mnemonic-named label already has.
static token_table statement_tokens(const baron *b)
{
    return macros_statement_tokens(&b->macros);
}

// '=' is its own tiny table, lexed after an identifier at statement start (an assignment) and after an
// attribute name on a SECTION line. Defined here so both the assignment handler and handle_section can reach it.
static const token assign_token_entries[] = {
    {RC_STR("="), {.type = lexeme_type_assign}},
};
static const token_table assign_tokens = RC_VIEW(assign_token_entries);

// require_separator is shared with opcodes.c (declared in assemble.h); it reads the statement
// table to recognise the '}' that implicitly closes a one-liner, so it lives here with it.
parse_result require_separator(baron *b, cursor at)
{
    rc_str source = source_files_text(&b->source_files, at.source);
    lexer_result r = lexer_next(source, at.pos, statement_tokens(b));

    if (r.token.type == lexeme_type_terminator) {
        return (parse_result) { .next = r.next };
    }

    if (r.token.type == lexeme_type_closer && r.token.closer.id == closer_brace) {
        return (parse_result) { .next = at.pos };   // '}' closes the statement; parse_block consumes it
    }

    return syntax_error(b, error_type_expected_separator, at);
}

// The one place that projects baron into an expr_env: symbols from `scope`, the live PC of the current
// section, and the reference's own position (`at`) for the impure @- / @+ locals. Every directive / operand
// evaluates through here, so no call site rebuilds the environment and the expression parser never sees
// baron. The cursor's source+pos double as the parse start and the use site. Shared with opcodes.c.
expr_result eval(baron *b, cursor at, uint32_t scope, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    expr_env env = {
        .scopes         = &b->scopes,
        .scope_index    = scope,
        .pc             = sections_pc(&b->sections, b->current_section),
        .source         = at.source,
        .offset         = at.pos,
        .operand_tokens = functions_operand_tokens(&b->functions),   // base + a token per FUNCTION name
        .functions      = &b->functions,
        .sources        = &b->source_files,
        .call_depth     = &b->function_depth,
    };
    return expression_parse(src, at.pos, &env, &scratch);
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

// SKIP n - pad the object code with n zero bytes (advancing pc by n). A negative count would
// rewind the pointer, which we cannot do; the layout-dependent check is deferred to the final pass.
static parse_result handle_skip(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{

    expr_result e = eval(b, at, scope, scratch);
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
                    sections_skip(&b->sections, b->current_section, (uint32_t) arg.value);
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

    expr_result e = eval(b, at, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_make(e.value, flags.final, at.pos);
        switch (arg.type) {
            case int_argument_type_known: {
                uint32_t pc = sections_pc(&b->sections, b->current_section);
                if (arg.value < pc) {
                    semantic_error(b, flags, error_type_skip_backwards, cursor_at(at, at.pos));   // skip nothing
                }
                else {
                    sections_skip(&b->sections, b->current_section, (uint32_t) (arg.value - pc));
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

    expr_result e = eval(b, at, scope, scratch);
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
                    uint32_t rem = sections_pc(&b->sections, b->current_section) % n;
                    if (rem != 0) {
                        sections_skip(&b->sections, b->current_section, n - rem);
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

// SECTION name, key = expr, ... / ENDSECTION - a lexically scoped region of object code. The name is UNIQUE
// (a repeat is error_type_duplicate_section: a name identifies one output blob, and there is no
// concatenation). The attributes are `key = expr` pairs resolved here: the assembler acts on `org` (which
// sets this section's start address - else the running emission cursor simply continues) and stores every
// attribute on the section for the output utility to read out of the result. Sections nest: a child inherits
// its parent's attributes (its own keys override) and, on ENDSECTION, hands the cursor back so the parent
// resumes where the child left off. A SECTION does NOT open a naming scope - labels inside bind in the
// enclosing scope, exactly as an IF body does. A dead branch parses the whole block for its extent but
// creates nothing and emits nothing.
static parse_result handle_section(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The name is a bare identifier (a mnemonic-spelled name would lex as that opcode - the same limitation
    // labels have, and not worth a private name table).
    lexer_result nm = lexer_next(src, at.pos, statement_tokens(b));
    if (nm.token.type != lexeme_type_identifier) {
        return syntax_error(b, error_type_expected_section_name, cursor_at(at, at.pos));
    }

    // Create the section and switch into it (only when live). parent_section is restored on ENDSECTION; the
    // cursor flows: the child starts where the parent's pc sits (unless an `org` attribute jumps it), and on
    // close the parent resumes from the child's end.
    uint32_t parent_section = b->current_section;
    uint32_t child          = RC_INDEX_NONE;
    if (flags.active) {
        child = sections_make(&b->sections, nm.token.identifier.name);
        if (child == RC_INDEX_NONE) {
            return syntax_error(b, error_type_duplicate_section, cursor_at(at, at.pos));   // names are unique
        }
        // Inherit the parent's attributes (bar `org`, which is positional), then default the cursor to
        // continue from the parent; an explicit `org` attribute below overrides it.
        rc_view_attribute inherited = sections_attributes(&b->sections, parent_section);
        for (uint32_t i = 0; i < inherited.num; i++) {
            attribute a = rc_view_attribute_get(inherited, i);
            if (!rc_str_is_equal_insensitive(a.key, RC_STR("org"))) {
                sections_add_attribute(&b->sections, child, a.key, a.v, a.at);
            }
        }
        sections_org(&b->sections, child, sections_pc(&b->sections, parent_section));
        b->current_section = child;
    }

    // The attribute list: `, key = expr` pairs to the end of the SECTION line. Parsed structurally even in a
    // dead branch (to reach the body); applied only when live.
    uint32_t pos = nm.next;
    bool unresolved = false;
    while (true) {
        lexer_result comma = lexer_next(src, pos, statement_tokens(b));
        if (comma.token.type != lexeme_type_comma) {
            break;   // no more attributes - comma.token is the line terminator, left to require_separator
        }
        lexer_result key = lexer_next(src, comma.next, statement_tokens(b));
        if (key.token.type != lexeme_type_identifier) {
            b->current_section = parent_section;
            return syntax_error(b, error_type_unexpected_token, cursor_at(at, comma.next));   // want an attribute name
        }
        lexer_result eq = lexer_next(src, key.next, assign_tokens);
        if (eq.token.type != lexeme_type_assign) {
            b->current_section = parent_section;
            return syntax_error(b, error_type_expected_assign, cursor_at(at, key.next));
        }
        expr_result e = eval(b, cursor_at(at, eq.next), scope, scratch);
        if (e.error != expr_error_none) {
            b->current_section = parent_section;
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        if (child != RC_INDEX_NONE) {
            sections_add_attribute(&b->sections, child, key.token.identifier.name, e.value, cursor_at(at, comma.next));
            if (rc_str_is_equal_insensitive(key.token.identifier.name, RC_STR("org"))) {
                int_argument arg = int_argument_make(e.value, flags.final, eq.next);
                switch (arg.type) {
                    case int_argument_type_known:
                        sections_org(&b->sections, child, (uint32_t) (arg.value & 0xFFFF));
                        break;
                    case int_argument_type_unresolved:
                        unresolved = true;   // a forward-referenced org owes another pass; cursor stays for now
                        break;
                    case int_argument_type_error:
                        semantic_error(b, flags, arg.error, cursor_at(at, eq.next));
                        break;
                }
            }
        }
        pos = e.next;
    }

    // Close the SECTION line, parse the body up to ENDSECTION, then restore the parent (threading the cursor).
    parse_result sep = require_separator(b, cursor_at(at, pos));
    if (sep.fatal) {
        b->current_section = parent_section;
        return sep;
    }

    parse_result body = parse_block(b, cursor_at(at, sep.next), scope, flags, scratch);
    body.unresolved |= unresolved;

    if (child != RC_INDEX_NONE) {
        sections_org(&b->sections, parent_section, sections_pc(&b->sections, child));   // parent resumes from the child's end
    }
    b->current_section = parent_section;

    if (body.fatal) {
        return body;
    }

    lexer_result cl = lexer_next(src, body.next, statement_tokens(b));
    if (cl.token.type == lexeme_type_closer && cl.token.closer.id == closer_endsection) {
        body.next = cl.next;
        return fold(body, require_separator(b, cursor_at(at, body.next)));
    }
    return fold(body, syntax_error(b, error_type_unclosed_section, cursor_at(at, body.next)));   // a foreign closer / EOF
}

// Add one ZPRESERVE value's zero-page bytes to the reserve set. Mirrors emit_data's descent: a range is
// enumerated, a list is flattened, and a scalar is one byte - but it must land in the zero page ($00-$FF).
// A forward reference defers (marks unresolved, reserves nothing this pass); a non-numeric or out-of-page
// value is a semantic error. A dead branch reserves nothing.
static parse_result zpreserve_add(baron *b, value v, parse_flags flags, cursor at, rc_arena scratch)
{
    if (!flags.active) {
        return (parse_result) {0};
    }

    if (value_is_range(v)) {
        return zpreserve_add(b, range_to_list(v.range, &scratch), flags, at, scratch);
    }

    if (value_is_list(v)) {
        parse_result acc = {0};
        for (uint32_t i = 0; i < v.list.num; i++) {
            acc = fold(acc, zpreserve_add(b, rc_view_value_get(v.list, i), flags, at, scratch));
        }
        return acc;
    }

    // A numeric, a forward reference, or an error (a string / list leaf lands here as operand_not_numeric).
    int_argument arg = int_argument_make(v, flags.final, at.pos);
    if (arg.type == int_argument_type_error) {
        semantic_error(b, flags, arg.error, at);
        return (parse_result) {0};
    }
    if (arg.type == int_argument_type_unresolved) {
        return (parse_result) {.unresolved = true};   // a forward-referenced address settles on a later pass
    }
    if (arg.value < 0 || arg.value >= zeropage_size) {
        semantic_error(b, flags, error_type_reserve_not_zeropage, at);
        return (parse_result) {0};
    }
    zeropage_reserve(&b->zeropage, (uint32_t) arg.value);
    return (parse_result) {0};
}

// ZPRESERVE <list> - declare the zero-page bytes the ZPAUTO1/ZPAUTO2 allocator may draw from, and (by its mere
// presence) ENABLE the whole feature. A comma-separated list of values, each a zero-page address or a range
// of them (the same operand shape as EQUB); every value must fall within $00-$FF. The set is global and
// rebuilt each pass. A dead branch parses the operand but reserves nothing and does not enable.
static parse_result handle_zpreserve(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;

    if (flags.active) {
        zeropage_enable(&b->zeropage);   // ZPRESERVE turns the feature on even if the list is empty
    }

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        parse_result ra = zpreserve_add(b, e.value, flags, cursor_at(at, pos), scratch);
        unresolved |= ra.unresolved;

        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;   // end of the list: a terminator or '}'
    }
}

// ZPAUTO1 <names> / ZPAUTO2 <names> - declare 1- or 2-byte zero-page variables, auto-allocated from the ZPRESERVE
// set. Each name binds an ordinary scoped symbol (so `LDA foo` sizes as zero page and `routine.foo` resolves
// from outside, all for free) to a fixed PLACEHOLDER address; the real byte is assigned later, at the
// allocation phase. The variable's width + identity are recorded in the zeropage var registry, but only on
// the single final pass (the settling passes need just the placeholder binding for layout to converge). ZPAUTO
// is meaningless without a ZPRESERVE first: we flag that, but still bind the names so references do not cascade
// into undefined-symbol errors. A dead branch removes only the binding it owns, like a dead label.
static parse_result handle_zpauto(baron *b, cursor at, uint32_t scope, parse_flags flags, uint8_t width, rc_arena scratch)
{
    (void) scratch;
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;

    if (flags.active && !zeropage_is_enabled(&b->zeropage)) {
        semantic_error(b, flags, error_type_var_without_reserve, cursor_at(at, at.pos));
    }

    while (true) {
        lexer_result nm = lexer_next(src, pos, statement_tokens(b));
        if (nm.token.type != lexeme_type_identifier) {
            return syntax_error(b, error_type_expected_var_name, cursor_at(at, pos));   // no name: malformed
        }
        rc_str name = nm.token.identifier.name;
        cursor def  = cursor_at(at, pos);

        if (is_dotted(name)) {
            // A dotted name is a legal token but not a legal ZPAUTO - record it and consume just the name.
            semantic_error(b, flags, error_type_expected_var_name, def);
        }
        else if (rc_str_is_equal_insensitive(name, RC_STR("a"))) {
            // `a` alone stays forbidden: it collides with accumulator addressing, so `ASL a` would read as
            // `ASL A` (accumulator) and silently drop the variable - and it even changes size (1 byte, not 2),
            // so no patch could rescue it. X and Y are safe now: they are registers only after a comma (an
            // index position a base operand never occupies), so `STA x` / `LDA (y),Y` attribute correctly.
            semantic_error(b, flags, error_type_zpauto_register_name, def);
        }
        else if (flags.active) {
            symbol_status st = scopes_set_symbol(
                &b->scopes, scope, name,
                value_make_numeric((double) zeropage_var_placeholder), def);

            if (st == symbol_status_duplicate) {
                semantic_error(b, flags, error_type_duplicate_symbol, def);
                cursor original = scopes_symbol_def(&b->scopes, scope, name);
                if (!cursor_is_none(original)) {
                    semantic_error(b, flags, error_type_original_definition, original);
                }
            }
            else if (flags.final) {
                // Record the vreg. Its identity is the (scope, def) pair: a macro / FOR body shares one def
                // across every instantiation, but each runs in its own child scope, so each instance becomes a
                // distinct variable here - exactly as two sibling blocks declaring the same name would.
                zeropage_add_var(&b->zeropage, name, scope, width, def);
            }
        }
        else if (cursor_is_equal(scopes_symbol_def(&b->scopes, scope, name), def)) {
            scopes_remove_symbol(&b->scopes, scope, name);   // dead branch: clear only our own binding
        }

        lexer_result lr = lexer_next(src, nm.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another name follows
        }
        return require_separator(b, cursor_at(at, nm.next));
    }
}

static parse_result handle_zpauto1(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_zpauto(b, at, scope, flags, 1, scratch);
}
static parse_result handle_zpauto2(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_zpauto(b, at, scope, flags, 2, scratch);
}

// UNREACHABLE - a zero-byte assertion, placed right after an always-taken branch, that control cannot fall
// through to this point. The allocator's CFG would otherwise wire the branch's fall-through edge and treat
// whatever is live down that dead path as live across the branch, pinning bytes needlessly. Recording this
// pc lets zeropage_finalize prune that one edge. It is TRUSTED - a wrong UNREACHABLE (a fall-through that
// really can happen) is one of the few ways to defeat the certainty contract, but it is the programmer's
// explicit promise. Only meaningful on the final pass, and only with the feature enabled.
static parse_result handle_unreachable(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    (void) scope;
    (void) scratch;
    if (flags.final && flags.active && zeropage_is_enabled(&b->zeropage)) {
        zeropage_add_cflow(&b->zeropage, (zp_cflow) {
            .site   = sections_pc(&b->sections, b->current_section),
            .target = RC_INDEX_NONE,
            .kind   = zp_cflow_unreachable,
            .at     = cursor_at(at, at.pos),
        });
    }
    return require_separator(b, at);
}

// The shared body of CANCALL / CANJUMP: parse a comma-separated list of target addresses and record one cflow
// of `kind` per target, sited on the last recorded instruction (the JSR / JMP this annotation qualifies) - but
// only when that instruction's flow is `expect_flow`, so a stray CANCALL after a JMP (or vice versa) binds to
// nothing rather than mis-annotating. Only the final pass records instructions, so only then is there a site;
// the settling passes still parse the list so the statement stays well-formed. A forward target defers.
static parse_result handle_can_targets(baron *b, cursor at, uint32_t scope, parse_flags flags,
                                       zp_flow expect_flow, zp_cflow_kind kind, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;

    uint32_t site = RC_INDEX_NONE;
    if (flags.final) {
        uint32_t ni = zeropage_insn_count(&b->zeropage);
        if (ni > 0) {
            zp_insn last = zeropage_insn_get(&b->zeropage, ni - 1);
            if (last.flow == expect_flow) {
                site = last.pc;
            }
        }
    }

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        if (flags.final && flags.active && zeropage_is_enabled(&b->zeropage) && site != RC_INDEX_NONE) {
            int_argument arg = int_argument_make(e.value, flags.final, pos);
            switch (arg.type) {
                case int_argument_type_known:
                    zeropage_add_cflow(&b->zeropage, (zp_cflow) {
                        .site   = site,
                        .target = (uint32_t) (arg.value & 0xFFFF),
                        .kind   = (uint8_t) kind,
                        .at     = cursor_at(at, pos),
                    });
                    break;
                case int_argument_type_unresolved:
                    unresolved = true;   // a forward target: settle it next pass
                    break;
                case int_argument_type_error:
                    semantic_error(b, flags, arg.error, cursor_at(at, arg.error_at));
                    break;
            }
        }

        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another target follows
        }
        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;
    }
}

// CANCALL <targets> - the programmer declares the real destination(s) of the JSR immediately preceding it (a
// self-modified operand, or a dispatch the analysis cannot follow). Without it, such a call has an unknown
// footprint and a value held live across it is refused (error_type_zpauto_across_call); with it, the callee
// footprint is bounded by the union of the named routines. TRUSTED, like UNREACHABLE.
static parse_result handle_cancall(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_can_targets(b, at, scope, flags, zp_flow_call, zp_cflow_cancall, scratch);
}

// CANJUMP <targets> - the programmer declares the possible destinations of the computed / indirect JMP
// immediately preceding it (a jump table). Without it, the jump reaches code the CFG cannot follow and, with
// variables live, is refused (error_type_zpauto_computed_flow); with it, the CFG wires every named target as a
// real successor edge, so liveness follows control to each one. TRUSTED, like UNREACHABLE.
static parse_result handle_canjump(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_can_targets(b, at, scope, flags, zp_flow_jump, zp_cflow_canjump, scratch);
}

// Emit `bits` as `width` little-endian bytes into the current section.
static void emit_le(baron *b, uint64_t bits, uint32_t width)
{
    for (uint32_t i = 0; i < width; i++) {
        sections_emit_u8(&b->sections, b->current_section, (uint8_t) (bits >> (8 * i)));
    }
}

// Emit one value into the current section as `width`-byte little-endian units, for EQUB/EQUW/EQUD (width
// 1/2/4). A string goes character by character (each char widened to `width` bytes); a range is enumerated;
// a list is descended (so nested lists and ranges flatten out); anything else is one `width`-byte unit via
// int_argument_make, where a forward reference emits a zero placeholder of the right size and asks for
// another pass. Returns .error/.error_at on failure and .unresolved when a value defers (its .next is
// unused). A dead branch emits nothing and raises nothing - mirroring inactive statements.
static parse_result emit_data(baron *b, value v, parse_flags flags, cursor at, uint32_t width, rc_arena scratch)
{
    if (!flags.active) {
        return (parse_result) {0};
    }

    if (value_is_string(v)) {
        for (uint32_t i = 0; i < v.string.len; i++) {
            emit_le(b, (uint8_t) v.string.data[i], width);
        }
        return (parse_result) {0};
    }

    if (value_is_range(v)) {
        return emit_data(b, range_to_list(v.range, &scratch), flags, at, width, scratch);   // enumerated to a list (or an error leaf)
    }

    if (value_is_list(v)) {
        parse_result acc = {0};
        for (uint32_t i = 0; i < v.list.num; i++) {
            acc = fold(acc, emit_data(b, rc_view_value_get(v.list, i), flags, at, width, scratch));
        }
        return acc;
    }

    // A numeric, a forward reference, or some other error value - one `width`-byte unit either way.
    int_argument arg = int_argument_make(v, flags.final, at.pos);
    if (arg.type == int_argument_type_error) {
        semantic_error(b, flags, arg.error, at);
        emit_le(b, 0, width);   // best-effort placeholder; keeps the size stable
        return (parse_result) {0};
    }
    if (arg.type == int_argument_type_unresolved) {
        emit_le(b, 0, width);   // placeholder of the right width; forces another pass
        return (parse_result) {.unresolved = true};
    }
    // We allow signed values, so the window is -(2^(8*width)-1) .. (2^(8*width)-1); recorded once settled.
    int64_t limit = (int64_t) ((1ull << (8 * width)) - 1);
    if (arg.value < -limit || arg.value > limit) {
        semantic_error(b, flags, error_type_value_out_of_range, at);
    }
    emit_le(b, (uint64_t) arg.value, width);
    return (parse_result) {0};
}

// EQUB / EQUS (width 1) / EQUW (2) / EQUD (4): a comma-separated list of values, each emitted as `width`-byte
// little-endian units (see emit_data). All take numbers, strings, ranges and lists alike; EQUS is an EQUB alias.
static parse_result handle_equ(baron *b, cursor at, uint32_t scope, parse_flags flags, uint32_t width, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        parse_result em = emit_data(b, e.value, flags, cursor_at(at, pos), width, scratch);
        if (em.fatal) {
            return em;
        }
        unresolved |= em.unresolved;

        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;   // end of the list: a terminator or '}'
    }
}

// The width-specialised entry points named in the statement table. EQUS is an alias of EQUB.
static parse_result handle_equb(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_equ(b, at, scope, flags, 1, scratch);
}
static parse_result handle_equw(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_equ(b, at, scope, flags, 2, scratch);
}
static parse_result handle_equd(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    return handle_equ(b, at, scope, flags, 4, scratch);
}

static parse_result handle_label(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The label name. A name spelled exactly like a mnemonic would lex as that opcode (the same
    // limitation an assignment target has at statement start) - acceptable, and not worth a
    // private name table.
    lexer_result nm = lexer_next(src, at.pos, statement_tokens(b));
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
            value_make_numeric((double)sections_pc(&b->sections, b->current_section)),
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
    else if (cursor_is_equal(scopes_symbol_def(&b->scopes, scope, name), at)) {
        // Dead branch: clear only the binding THIS label owns (its def is our cursor), so a live sibling
        // branch's - or an outer statement's - like-named binding survives.
        r.changed = scopes_remove_symbol(&b->scopes, scope, name);
    }

    // What follows decides the label's shape. A '{' - optionally one separator away, so it may sit
    // on the next line - makes the label name a scope. Anything else is not the label's to parse: we
    // drop back to the parent loop, which takes the next token as its own statement (or, on a '}',
    // closes the block). The label never owns the statement that follows it.
    lexer_result nb = lexer_next(src, r.next, statement_tokens(b));
    
    if (nb.token.type == lexeme_type_terminator) {
        lexer_result after = lexer_next(src, nb.next, statement_tokens(b));
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

// A local label '.@': an anonymous marker at the current PC that @- / @+ branch to. We reuse anon_scope_key
// to give it an unspellable per-position key in the ordinary symbol table, so it converges, clears on a dead
// branch, and defers a forward @+ exactly as a named label would. Its cursor is unique per '.@', so it can
// never be a duplicate. A '.@' is a whole statement with nothing following it, so we consume just the token
// and leave the rest of the line to the statement loop.
static parse_result handle_local_label(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    (void) scratch;
    char storage[64];
    rc_mstr key = anon_scope_key(storage, sizeof storage, at);

    parse_result r = {.next = at.pos};
    if (flags.active) {
        symbol_status st = scopes_set_symbol(
            &b->scopes,
            scope,
            key.view,
            value_make_numeric((double) sections_pc(&b->sections, b->current_section)),
            at);
        r.changed = (st == symbol_status_changed);   // a moved local label drives another pass, like any label
    }
    else if (cursor_is_equal(scopes_symbol_def(&b->scopes, scope, key.view), at)) {
        // Dead branch: clear only the binding this local label owns. Its @source:pos key is unique per
        // position, so the guard is a no-op here, but we keep it uniform with the named-label / assignment cases.
        r.changed = scopes_remove_symbol(&b->scopes, scope, key.view);
    }
    return r;
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

    expr_result e = eval(b, at, scope, scratch);
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

    lexer_result t = lexer_next(src, acc.next, statement_tokens(b));

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
        t = lexer_next(src, acc.next, statement_tokens(b));
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

// `name` is the identifier and `pos` sits just past it; an assignment defines a symbol and
// emits nothing.
static parse_result handle_assignment(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_str name, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    lexer_result eq = lexer_next(src, at.pos, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return syntax_error(b, error_type_expected_assign, cursor_at(at, at.pos));   // not an assignment: malformed
    }

    expr_result e = eval(b, cursor_at(at, eq.next), scope, scratch);
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
    else if (cursor_is_equal(scopes_symbol_def(&b->scopes, scope, name), at)) {
        // Dead branch: clear only the binding THIS assignment owns (def is our cursor), so a live sibling
        // branch - IF TRUE:x=2:ELSE:x=3:ENDIF - or an outer binding of the same name is not clobbered.
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
    lexer_result var = lexer_next(src, at.pos, statement_tokens(b));
    if (var.token.type != lexeme_type_identifier || is_dotted(var.token.identifier.name)) {
        return syntax_error(b, error_type_expected_label_name, cursor_at(at, at.pos));
    }
    rc_str name = var.token.identifier.name;

    lexer_result eq = lexer_next(src, var.next, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return syntax_error(b, error_type_expected_assign, cursor_at(at, var.next));
    }

    expr_result e = eval(b, cursor_at(at, eq.next), scope, scratch);
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
    lexer_result t = lexer_next(src, acc.next, statement_tokens(b));
    if (t.token.type == lexeme_type_closer && t.token.closer.id == closer_next) {
        acc.next = t.next;
        return fold(acc, require_separator(b, cursor_at(at, acc.next)));
    }
    return fold(acc, syntax_error(b, error_type_unclosed_for, cursor_at(at, acc.next)));   // '}' / EOF / foreign keyword before NEXT
}


// ---- INCLUDE ----

// INCLUDE "file" splices another source in at this point - textually, so its code emits into the current
// section at the current pc and its symbols bind into the current scope (no scope of its own). We re-parse
// the included file every pass, exactly like the rest of the statement stream, so forward references cross
// the boundary freely. The filename is resolved relative to THIS file's directory (see file_path_resolve).
static parse_result handle_include(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{

    // The filename is a string operand, evaluated on the spot - we load the file this very pass, so a
    // forward-referenced name is no use to us (it stays an error value, and we grumble about it below).
    expr_result e = eval(b, at, scope, scratch);
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

// INCBIN "file" - splice a binary file's bytes into the current section. Its size never changes across passes,
// so on the settling passes we do NOT read the file at all: rc_file_size tells us how many bytes it will be and
// we just advance pc by that much (sections_skip pads with zeroes). Only on the FINAL pass do we actually load
// it and emit the real bytes (via emit_data, reusing EQUB's per-byte path over a string of the contents). The
// filename is a string operand resolved relative to the includer, exactly like INCLUDE; a forward-referenced
// name defers like a forward address. A missing / unreadable file is FATAL (there is no sensible recovery - the
// output would be the wrong size), so it unwinds the whole assemble rather than accumulating.
static parse_result handle_incbin(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    expr_result e = eval(b, at, scope, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    parse_result pulled = {0};
    if (flags.active) {
        if (value_is_string(e.value)) {
            rc_str base = source_files_name(&b->source_files, at.source);
            rc_str path = file_path_resolve(base, e.value.string, &scratch);
            if (flags.final) {
                // Final pass: load the file and emit its bytes for real (one byte at a time, via emit_data).
                rc_file_load_binary_result f = rc_file_load_binary(path, 0, &scratch);
                if (f.error != RC_FILE_OK) {
                    return syntax_error(b, error_type_source_load, cursor_at(at, at.pos));
                }
                rc_str bytes = {.data = (const char *) f.contents.view.data, .len = f.contents.view.num};
                pulled = emit_data(b, value_make_string(bytes), flags, cursor_at(at, at.pos), 1, scratch);
            }
            else {
                // Settling passes: reserve the file's size without reading it, so pc / later labels are right.
                rc_file_size_result sz = rc_file_size(path);
                if (sz.error != RC_FILE_OK) {
                    return syntax_error(b, error_type_source_load, cursor_at(at, at.pos));
                }
                sections_skip(&b->sections, b->current_section, sz.size);
            }
        }
        else if (value_is_error(e.value) && e.value.error == error_type_unknown_symbol) {
            // A forward-referenced filename: defer, like INCLUDE. Still unknown on the final pass means it never
            // binds (a name defined only where it cannot be seen in time), so we call it out then.
            if (flags.final) {
                semantic_error(b, flags, error_type_undefined_symbol, cursor_at(at, at.pos));
            }
            else {
                pulled.unresolved = true;
            }
        }
        else {
            semantic_error(b, flags, error_type_expected_filename, cursor_at(at, at.pos));
        }
    }

    return fold(pulled, require_separator(b, cursor_at(at, e.next)));
}


// ---- MACRO ----

// MACRO name [signature] : ...body... : ENDMACRO, evaluated each pass. The name/token is registered BEFORE
// the body is scanned, so the body may call the macro itself (self-recursion); mutual recursion needs a
// forward declaration - an empty body - so the partner's name is a token in time. The body is captured as a
// cursor and only ever PARSED at invocation; here we scan it inactively (the trick FOR uses to locate NEXT),
// which finds ENDMACRO through the real parser - handling multi-line list literals and nested IF/FOR/{} -
// without expanding anything. The definition emits nothing itself.
static parse_result handle_macro(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The name, from the STATIC base table (so an already-defined macro name reads as a plain identifier
    // here, not a call - which is what lets an overload or a fill-in reuse the one name entry).
    lexer_result nm = lexer_next(src, at.pos, base_statement_tokens);
    if (nm.token.type != lexeme_type_identifier) {
        error_type code = (nm.token.type == lexeme_type_terminator)
                              ? error_type_expected_macro_name    // MACRO with nothing after it
                              : error_type_macro_name_reserved;    // a mnemonic / keyword / constant / closer
        return syntax_error(b, code, cursor_at(at, at.pos));
    }
    rc_str name = nm.token.identifier.name;
    if (is_dotted(name)) {
        return syntax_error(b, error_type_macro_name_reserved, cursor_at(at, at.pos));   // a dotted name is not a macro name
    }

    // Register the name now (idempotent), before the body is scanned, so a self-call inside lexes as a macro.
    uint32_t index = macros_index_for_name(&b->macros, name);

    // The signature: slots up to the terminator MACRO insists on. Built in the manager arena, stored as a view.
    rc_array_macro_slot slots = rc_array_macro_slot_make(8, b->macros.arena);
    uint32_t pos = nm.next;
    while (true) {
        lexer_result s = lexer_next(src, pos, base_statement_tokens);
        if (s.token.type == lexeme_type_terminator) {
            pos = s.next;   // the required separator; the body starts here
            break;
        }
        if (s.token.type == lexeme_type_identifier) {
            rc_array_macro_slot_push(&slots,
                (macro_slot) {.type = macro_slot_param, .name = s.token.identifier.name}, b->macros.arena);
        }
        else if (s.token.type == lexeme_type_string_literal || s.token.type == lexeme_type_escaped_string_literal) {
            uint32_t id = macros_intern_literal(&b->macros, index, s.token.string_literal.ref);
            rc_array_macro_slot_push(&slots,
                (macro_slot) {.type = macro_slot_literal, .literal_id = id}, b->macros.arena);
        }
        else if (s.token.type == lexeme_type_comma) {
            // The comma is its own slot: the lexer yields lexeme_type_comma intrinsically (never via a token
            // table), so a comma can never be matched through the literal table - it is handled directly.
            rc_array_macro_slot_push(&slots,
                (macro_slot) {.type = macro_slot_comma}, b->macros.arena);
        }
        else if (s.token.type == lexeme_type_closer) {
            return syntax_error(b, error_type_expected_separator, cursor_at(at, pos));   // ENDMACRO / '}' before a separator
        }
        else {
            return syntax_error(b, error_type_unquoted_macro_token, cursor_at(at, pos));   // bare punctuation must be quoted
        }
        pos = s.next;
    }
    cursor body = cursor_at(at, pos);

    // Empty body == a forward declaration: true iff the first meaningful token is ENDMACRO.
    lexer_result peek = lexer_next(src, body.pos, statement_tokens(b));
    while (peek.token.type == lexeme_type_terminator && !lexer_at_end(src, peek.next)) {
        peek = lexer_next(src, peek.next, statement_tokens(b));
    }
    bool defined = !(peek.token.type == lexeme_type_closer && peek.token.closer.id == closer_endmacro);

    // Scan the body inactively to find its ENDMACRO. Nested calls consume their arguments but do not expand
    // (see handle_macro_invocation), so the scan never recurses and always stops at this macro's ENDMACRO.
    parse_result scan = parse_block(b, body, scope, (parse_flags) {flags.final, false}, scratch);
    if (scan.fatal) {
        return scan;   // a structurally broken body aborts, reported at the definition
    }
    lexer_result end = lexer_next(src, scan.next, statement_tokens(b));
    if (!(end.token.type == lexeme_type_closer && end.token.closer.id == closer_endmacro)) {
        return syntax_error(b, error_type_unclosed_macro, cursor_at(at, scan.next));   // a foreign closer / EOF first
    }

    // Register the signature, but only when the definition is actually reached: a dead-branch definition, or
    // one met while inactively scanning another macro's body, registers nothing. Reconcile against overloads.
    if (flags.active) {
        macro_add_status st = macros_add_signature(&b->macros, index, slots.view, body, defined);
        if (st == macro_add_duplicate) {
            semantic_error(b, flags, error_type_duplicate_signature, cursor_at(at, at.pos));
        }
    }

    // ENDMACRO insists on a separator after it, like NEXT / ENDIF.
    return require_separator(b, cursor_at(at, end.next));
}

// The header parens - not statement tokens, so a small dedicated table (identifiers / commas are intrinsic).
static const token func_paren_entries[] = {
    {RC_STR("("), {.type = lexeme_type_open_paren}},
    {RC_STR(")"), {.type = lexeme_type_close_paren}},
};
static const token_table func_paren_tokens = RC_VIEW(func_paren_entries);

// FUNCTION name(params) [ : body ] = return-expr, evaluated each pass. The name/token is registered BEFORE
// the body is scanned, so the body may call the function itself (self-recursion). The definition is a
// STATEMENT (it emits nothing); the body's value semantics live in the evaluator (expression.c), which we
// call here only to locate the top-level '=' return and learn whether this is a real body or a forward
// declaration (an empty body plus an empty return expression).
static parse_result handle_function(baron *b, cursor at, uint32_t scope, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The name, from the STATIC base table (so an already-defined function name reads as a plain identifier
    // here, not a call - which is what lets an overload or a fill-in reuse the one name entry).
    lexer_result nm = lexer_next(src, at.pos, base_statement_tokens);
    if (nm.token.type != lexeme_type_identifier) {
        error_type code = (nm.token.type == lexeme_type_terminator)
                              ? error_type_expected_function_name
                              : error_type_function_name_reserved;
        return syntax_error(b, code, cursor_at(at, at.pos));
    }
    rc_str name = nm.token.identifier.name;
    if (is_dotted(name)) {
        return syntax_error(b, error_type_function_name_reserved, cursor_at(at, at.pos));
    }
    // A builtin operand call (abs(, lo(, shape() bakes its '(' into its token, so lexing the name here -
    // where the header's '(' follows it - matches such a builtin and rejects the collision. Use the STATIC
    // base operand table, not the dynamic one: reusing a prior USER function's name is fine (overload /
    // forward-decl fill-in), only a builtin clash is reserved.
    lexer_result op = lexer_next(src, at.pos, expression_operand_base());
    if (op.token.type == lexeme_type_unary_op || op.token.type == lexeme_type_function) {
        return syntax_error(b, error_type_function_name_reserved, cursor_at(at, at.pos));
    }

    // Register the name now (idempotent), before the body is scanned, so a self-call inside lexes as a call.
    uint32_t index = functions_index_for_name(&b->functions, name);

    // The parameter list: '(' identifiers (comma-separated) ')'. Empty is allowed (a niladic function).
    lexer_result lp = lexer_next(src, nm.next, func_paren_tokens);
    if (lp.token.type != lexeme_type_open_paren) {
        return syntax_error(b, error_type_expected_function_params, cursor_at(at, nm.next));
    }
    rc_array_rc_str params = rc_array_rc_str_make(8, b->functions.arena);
    uint32_t pos = lp.next;
    lexer_result t = lexer_next(src, pos, func_paren_tokens);
    if (t.token.type != lexeme_type_close_paren) {
        while (true) {
            if (t.token.type != lexeme_type_identifier) {
                return syntax_error(b, error_type_expected_function_params, cursor_at(at, pos));
            }
            rc_array_rc_str_push(&params, t.token.identifier.name, b->functions.arena);
            pos = t.next;
            t = lexer_next(src, pos, func_paren_tokens);
            if (t.token.type == lexeme_type_close_paren) {
                break;
            }
            if (t.token.type != lexeme_type_comma) {
                return syntax_error(b, error_type_expected_function_params, cursor_at(at, pos));
            }
            pos = t.next;
            t = lexer_next(src, pos, func_paren_tokens);
        }
    }
    cursor body = cursor_at(at, t.next);   // just past ')'

    // Scan the body (in the evaluator) to find the top-level '=' return and its extent. The scan is inactive
    // and side-effect-free (nested calls do not execute), and self-calls find no signature yet, so it always
    // terminates. It also reports whether this is a real body or a forward declaration (empty body + return).
    expr_env env = {
        .scopes         = &b->scopes,
        .scope_index    = scope,
        .pc             = sections_pc(&b->sections, b->current_section),
        .source         = at.source,
        .offset         = body.pos,
        .operand_tokens = functions_operand_tokens(&b->functions),
        .functions      = &b->functions,
        .sources        = &b->source_files,
        .call_depth     = &b->function_depth,
    };
    function_body_scan fs = expression_scan_function_body(src, body.pos, &env, &scratch);
    if (fs.error != error_type_none) {
        return syntax_error(b, fs.error, cursor_at(at, fs.error_at));   // a structurally broken body aborts
    }

    // Register the signature, but only when the definition is actually reached: a dead-branch definition, or
    // one met while inactively scanning another body, registers nothing. Reconcile against overloads by arity.
    if (flags.active) {
        function_add_status st = functions_add_signature(&b->functions, index, params.view, body, scope, fs.defined);
        if (st == function_add_duplicate) {
            semantic_error(b, flags, error_type_duplicate_function, cursor_at(at, at.pos));
        }
    }

    // The return expression insists on a following separator, like any statement.
    return require_separator(b, cursor_at(at, fs.next));
}

// Try to match one overload against the call text at `at`. Returns true (and sets *end, the cursor at the
// trailing separator) when every slot matches and the statement then ends. Literal slots are recognised via
// the macro's OWN literal table; parameter slots consume one expression in `scope` - its value is ignored
// here, so matching is purely structural and a forward-referenced argument matches like any other (the choice
// of overload is thus identical every pass). A parse failure (no operand where one is needed) fails the fit.
static bool macro_try_match(baron *b, rc_str src, cursor at, macro *m, macro_signature sig,
                            uint32_t scope, uint32_t *end, rc_arena scratch)
{
    uint32_t pos = at.pos;
    for (uint32_t k = 0; k < sig.slots.num; k++) {
        macro_slot slot = rc_view_macro_slot_get(sig.slots, k);
        if (slot.type == macro_slot_literal) {
            lexer_result lr = lexer_next(src, pos, m->literal_table.view);
            if (lr.token.type != lexeme_type_macro_literal || lr.token.macro_literal.id != slot.literal_id) {
                return false;
            }
            pos = lr.next;
        }
        else if (slot.type == macro_slot_comma) {
            lexer_result lr = lexer_next(src, pos, m->literal_table.view);
            if (lr.token.type != lexeme_type_comma) {
                return false;
            }
            pos = lr.next;
        }
        else {
            expr_result e = eval(b, cursor_at(at, pos), scope, scratch);
            if (e.error != expr_error_none) {
                return false;   // no operand here (or a broken one): this overload does not fit
            }
            pos = e.next;
        }
    }
    lexer_result term = lexer_next(src, pos, statement_tokens(b));
    if (term.token.type == lexeme_type_terminator
        || (term.token.type == lexeme_type_closer && term.token.closer.id == closer_brace)) {
        *end = pos;
        return true;
    }
    return false;
}

// name arg1, arg2 - a macro call. Match an overload, then (when live) stamp its body out into a fresh child
// scope with the arguments bound as symbols. Mirrors handle_for's body re-walk and handle_include's error
// breadcrumb. An inactive call consumes its arguments but expands nothing - which is what makes a recursive
// call terminate once its base-case branch goes inactive.
static parse_result handle_macro_invocation(baron *b, cursor at, uint32_t scope, parse_flags flags, uint32_t macro_index, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    macro *m = macros_at(&b->macros, macro_index);

    // Match an overload: sorted so tokens beat expressions, first full match wins. Matching runs whether or
    // not we are active - it validates the arguments and, above all, advances the cursor past the call.
    uint32_t chosen = RC_INDEX_NONE;
    uint32_t args_end = at.pos;
    for (uint32_t si = 0; si < m->signatures.num; si++) {
        uint32_t end;
        if (macro_try_match(b, src, at, m, rc_array_macro_signature_get(&m->signatures, si), scope, &end, scratch)) {
            chosen = si;
            args_end = end;
            break;
        }
    }

    if (chosen == RC_INDEX_NONE) {
        semantic_error(b, flags, error_type_no_matching_signature, cursor_at(at, at.pos));
        uint32_t p = at.pos;   // resync: skip to the end of the statement so parsing carries on
        while (true) {
            lexer_result lr = lexer_next(src, p, statement_tokens(b));
            if (lr.token.type == lexeme_type_terminator) {
                return (parse_result) {.next = lr.next};
            }
            if (lr.token.type == lexeme_type_closer) {
                return (parse_result) {.next = p};   // leave the closer for the caller
            }
            // A stray token the lexer cannot advance past (an unexpected char like a bare '#') returns
            // next == p; step over it by hand so the resync always makes progress to the terminator.
            p = lr.next > p ? lr.next : p + 1;
        }
    }

    // Copy out what the expansion needs BEFORE any body parse (which may register a macro and move the list).
    macro_signature sig = rc_array_macro_signature_get(&m->signatures, chosen);
    rc_view_macro_slot slots = sig.slots;
    cursor body = sig.body;
    bool defined = sig.defined;

    // Inactive (a dead branch, or a recursive call whose base case is met): arguments consumed, nothing more.
    if (!flags.active) {
        return require_separator(b, cursor_at(at, args_end));
    }

    // The depth guard is a runaway backstop; a bounded recursion unwinds well before it.
    if (b->macro_depth >= ASSEMBLE_MAX_MACRO_DEPTH) {
        semantic_error(b, flags, error_type_macro_too_deep, cursor_at(at, at.pos));
        return require_separator(b, cursor_at(at, args_end));   // expand nothing
    }

    // A fresh per-invocation child scope, keyed on the call site; the parent chain disambiguates recursion.
    char storage[64];
    rc_mstr key = anon_scope_key(storage, sizeof storage, at);
    uint32_t child = scopes_get_or_make_child(&b->scopes, scope, key.view);

    // Bind the parameters: re-evaluate each argument in the CALLER scope and set it as a symbol in the child.
    uint32_t pos = at.pos;
    for (uint32_t k = 0; k < slots.num; k++) {
        macro_slot slot = rc_view_macro_slot_get(slots, k);
        if (slot.type == macro_slot_literal || slot.type == macro_slot_comma) {
            pos = lexer_next(src, pos, m->literal_table.view).next;   // already matched; just step over it
        }
        else {
            expr_result e = eval(b, cursor_at(at, pos), scope, scratch);
            scopes_set_symbol(&b->scopes, child, slot.name, e.value, at);
            pos = e.next;
        }
    }

    // Expand the body in the child scope. Its errors point at the definition (parsed in place); if it raised
    // any, drop an "expanded from here" breadcrumb at the call site - the handle_include idiom.
    uint32_t errors_before = baron_error_count(b);
    b->macro_depth++;
    parse_result bodyr = parse_block(b, body, child, flags, scratch);
    b->macro_depth--;

    if (bodyr.fatal) {
        return bodyr;   // a broken body aborts the whole assemble
    }

    rc_str bsrc = source_files_text(&b->source_files, body.source);
    lexer_result be = lexer_next(bsrc, bodyr.next, statement_tokens(b));
    if (!(be.token.type == lexeme_type_closer && be.token.closer.id == closer_endmacro)) {
        return fold(bodyr, syntax_error(b, error_type_unclosed_macro, cursor_at(body, bodyr.next)));
    }

    if (baron_error_count(b) > errors_before) {
        semantic_error(b, flags, error_type_expanded_from, cursor_at(at, at.pos));
    }
    if (!defined) {
        semantic_error(b, flags, error_type_macro_not_defined, cursor_at(at, at.pos));   // invoked while only forward-declared
    }

    // Resume in the caller at the separator after the arguments; carry the body's convergence flags up.
    return fold(bodyr, require_separator(b, cursor_at(at, args_end)));
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
    lexer_result lr = lexer_next(src, at.pos, statement_tokens(b));

    switch (lr.token.type) {
        case lexeme_type_opcode:
            return opcode_parse(b, (mnemonic)lr.token.opcode.id, cursor_at(at, lr.next), scope, flags, scratch);
        case lexeme_type_keyword:
            return lr.token.keyword.handle(b, cursor_at(at, lr.next), scope, flags, scratch);
        case lexeme_type_macro:
            return handle_macro_invocation(b, cursor_at(at, lr.next), scope, flags, lr.token.macro.index, scratch);
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
        lexer_result lr = lexer_next(src, acc.next, statement_tokens(b));

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
    lexer_result lr = lexer_next(src, r.next, statement_tokens(b));

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
    lexer_result lr = lexer_next(src, r.next, statement_tokens(b));

    if (lr.token.type == lexeme_type_closer) {   // any closer at file scope has nothing to close
        return fold(r, syntax_error(b, (error_type) lr.token.closer.unexpected, cursor_at(at, lr.next)));
    }

    RC_ASSERT(lexer_at_end(src, r.next));   // parse_block had no other reason to stop
    return r;
}


// ---- the multi-pass driver ----

static parse_result run_pass(baron *b, uint32_t source, parse_flags flags, rc_arena scratch)
{
    // The per-pass arena backs sections, macros and functions - a fresh projection of the source each pass.
    // Reset it once, then rebuild all three stores into it (sections gets a fresh default section; the two
    // token tables are reseeded from their static bases, with room for per-name tokens).
    rc_arena_reset(b->per_pass);
    sections_reset(&b->sections);
    zeropage_reset(&b->zeropage);            // ZPRESERVE re-runs this pass and refills the (permanent) set
    b->current_section = sections_default;   // each pass re-derives the current section from the source
    b->include_depth   = 0;                  // balanced by handle_include, but a fatal unwind skips the decrement
    b->macro_depth     = 0;                  // ditto for macro expansion
    b->function_depth  = 0;                  // ditto for FUNCTION recursion (balanced by the evaluator)
    expression_reset_random();               // replay the same RND stream every pass, so RND can converge

    macros_reset(&b->macros, base_statement_tokens, 128);
    functions_reset(&b->functions, expression_operand_base(), 128);

    const uint32_t scope = 0;
    return parse_file(
        b,
        (cursor) {.source = source, .pos = 0},
        scope,
        flags,
        scratch
    );
}

// Post-convergence zero-page allocation. Layout has settled with every ZPAUTO reference sized as a
// placeholder zero-page access, so assigning a real byte and patching the operand cannot perturb size. The
// governing rule is CERTAINTY: this only patches a program it can prove correct, and turns anything it cannot
// into a clear diagnostic pointing the user at a fix or an annotation. Three refusals:
//   - a computed / indirect jump reaches code the CFG cannot follow (unknown_succ) -> error_type_zpauto_computed_flow;
//   - a variable is live across a JSR, whose callee footprint needs interprocedural analysis we do not yet do
//     -> error_type_zpauto_across_call;
//   - more simultaneously-live variables than reserved bytes -> error_type_zeropage_full (a spill).
// On any refusal it records the error(s) and patches nothing; run_passes then fails the assemble. Only a
// fully analysable, colourable program has its operands + symbols rewritten to real addresses.
static void zeropage_finalize(baron *b, rc_arena scratch)
{
    if (!zeropage_is_enabled(&b->zeropage)) {
        return;
    }
    zeropage_resolve_vregs(&b->zeropage);   // map each operand's def cursor to its vreg (registry now complete)
    rc_view_zp_insn insns = zeropage_insns(&b->zeropage);
    uint32_t nv = zeropage_var_count(&b->zeropage);

    bool refused = false;

    // Guard L (layout soundness): the CFG identifies a block purely by its address (block_at_pc), so two
    // instructions sharing a START address are indistinguishable - a branch/jump target could resolve to the
    // wrong one, a real edge be missed, and a live range wrongly shortened (a silently unsound allocation). An
    // address collision is the ONE layout hazard, and it is the only thing refused here. Everything with
    // distinct addresses is sound: multiple sections at DIFFERENT addresses - even a cross-section JSR resolves
    // by address and is analysed correctly; a spurious cross-section edge only lengthens a live range, never
    // shortens it. A collision arises when a section's cursor was dragged backwards onto its own emitted code
    // (a nested section with a low org threads back to it on close - named an org rewind) or two sections load
    // into the same address slot (named a section clash); we name which.
    // (Gated on nv > 0: with no variable there is nothing to allocate, so no hazard.)
    if (nv > 0) {
        uint32_t *pc_owner = rc_arena_alloc_zero_type(&scratch, uint32_t, 0x10000);   // 0 = free; else section+1
        for (uint32_t i = 0; i < insns.num; i++) {
            zp_insn n = rc_view_zp_insn_get(insns, i);
            if (n.pc >= 0x10000) {
                continue;   // a degenerate out-of-range pc; the CFG ignores it too (16-bit address space)
            }
            if (pc_owner[n.pc] != 0) {
                error_type e = (pc_owner[n.pc] - 1 == n.section) ? error_type_zpauto_org_rewind
                                                                 : error_type_zpauto_multi_section;
                baron_error(b, e, n.at);
                refused = true;
                break;
            }
            pc_owner[n.pc] = n.section + 1;
        }
    }

    // Guard 0 (the direct-addressing envelope): a variable reached by an indexed / indexed-indirect mode
    // (var,X, var,Y, (var,X)) touches var+index, a byte the allocator cannot see and may have given to another
    // variable. The whole packing is built on "a variable is reached only directly", so any such access is
    // unsound - refuse it rather than emit code that quietly corrupts a neighbour. (Checked per recorded insn
    // once vregs are resolved: a var_indexed insn that really names a ZPAUTO now has a vreg.)
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.vreg != RC_INDEX_NONE && n.var_indexed) {
            baron_error(b, error_type_zpauto_indexed_access, n.at);
            refused = true;
        }
    }

    // A broken layout or an out-of-envelope access is decided from the instruction stream alone - and building
    // a CFG over a stream we have just judged unanalysable would be meaningless. Bail before we do.
    if (refused) {
        return;
    }

    // Own working arenas: cfg + liveness results in `work`, their by-value scratch in `wscratch` (never the
    // same arena - see the scratch-aliasing lesson).
    rc_arena work     = rc_arena_make_default();
    rc_arena wscratch = rc_arena_make_default();

    rc_view_zp_cflow cflows = zeropage_cflows(&b->zeropage);

    // cfg_build applies the control-flow annotations itself: an UNREACHABLE prunes a branch's dead fall-through
    // edge, and a CANJUMP wires a computed JMP's declared targets (clearing the taint that would refuse it).
    cfg g       = cfg_build(insns, cflows, &work, wscratch);
    liveness lv = liveness_analyze(g, insns, nv, 0, &work, wscratch);

    // Guard 1: a computed / indirect jump (unknown_succ) leaves for code we cannot model. With any variable in
    // play we cannot prove it is not clobbered there, so refuse and ask for an annotation.
    for (uint32_t bi = 0; bi < g.blocks.num && nv > 0; bi++) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, bi);
        if (blk.unknown_succ) {
            zp_insn last = rc_view_zp_insn_get(insns, blk.first_insn + blk.num_insns - 1);
            baron_error(b, error_type_zpauto_computed_flow, last.at);
            refused = true;
        }
    }

    // Guard 2 / interprocedural interference: a variable live ACROSS a JSR is clobbered by the callee's whole
    // zero-page footprint, so it must interfere with every vreg the callee touches (transitively). Sweep each
    // block backward; at a call, the current live set is exactly what is live across it (a JSR touches no
    // variable of its own, so live-out at the call equals live-across). For each such call we compute the
    // callee footprint and add the edges - UNLESS it cannot be bounded (an untrackable target -> across_call)
    // or the call recurses (-> recursion): a value live across either cannot be statically placed, so refuse.
    rc_bitset live = {0};
    rc_bitset_resize(&live, nv, &wscratch);
    for (uint32_t bi = 0; bi < g.blocks.num; bi++) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, bi);
        rc_bitset_reset(&live);
        for (uint32_t v = 0; v < nv; v++) {
            if (liveness_is_live_out(&lv, bi, v)) {
                rc_bitset_set(&live, v);
            }
        }
        for (uint32_t k = blk.num_insns; k-- > 0; ) {
            zp_insn n = rc_view_zp_insn_get(insns, blk.first_insn + k);
            if (n.flow == zp_flow_call && rc_bitset_get_first_set(&live) != RC_INDEX_NONE) {
                // What this call reaches - CANCALL overrides an untrackable literal target with a declared set.
                footprint fp = footprint_of_call(g, insns, cflows, n, nv, &work, wscratch);
                if (fp.unknown_call) {
                    baron_error(b, error_type_zpauto_across_call, n.at);
                    refused = true;
                }
                else if (fp.recursive) {
                    baron_error(b, error_type_zpauto_recursion, n.at);
                    refused = true;
                }
                else {
                    // Every var live across this call interferes with every vreg the callee touches.
                    for (uint32_t c = rc_bitset_get_first_set(&live); c != RC_INDEX_NONE;
                         c = rc_bitset_get_next_set(&live, c + 1)) {
                        for (uint32_t t = rc_bitset_get_first_set(&fp.touched); t != RC_INDEX_NONE;
                             t = rc_bitset_get_next_set(&fp.touched, t + 1)) {
                            if (c != t) {
                                rc_bitset_set(&lv.interfere[c], t);
                                rc_bitset_set(&lv.interfere[t], c);
                            }
                        }
                    }
                }
            }
            if (n.vreg != RC_INDEX_NONE) {
                if (n.rw & vref_write) { rc_bitset_clear(&live, n.vreg); }
                if (n.rw & vref_read)  { rc_bitset_set(&live, n.vreg); }
            }
        }
    }

    if (!refused) {
        zp_coloring col = zp_color(&lv, zeropage_vars(&b->zeropage), zeropage_reserved(&b->zeropage),
                                   &work, wscratch);
        if (col.any_spilled) {
            for (uint32_t v = 0; v < nv; v++) {
                if (col.base[v] == RC_INDEX_NONE) {
                    baron_error(b, error_type_zeropage_full, zeropage_var_get(&b->zeropage, v).def);
                }
            }
        }
        else {
            // Patch each recorded operand: it was emitted with the placeholder base 0, so the byte held only
            // the intra-variable offset (0 for `var`, 1 for `var+1`); fold in the assigned base.
            for (uint32_t i = 0; i < insns.num; i++) {
                zp_insn n = rc_view_zp_insn_get(insns, i);
                if (n.vreg != RC_INDEX_NONE) {
                    sections_patch_add_u8(&b->sections, n.section, n.operand_offset, (uint8_t) col.base[n.vreg]);
                }
            }
            // Rewrite each variable's symbol from the placeholder to its real zero-page address.
            for (uint32_t v = 0; v < nv; v++) {
                zp_var var = zeropage_var_get(&b->zeropage, v);
                scopes_set_symbol(&b->scopes, var.scope, var.name,
                                  value_make_numeric((double) col.base[v]), var.def);
            }
        }
    }

    rc_arena_deinit(&wscratch);
    rc_arena_deinit(&work);
}

// Discard the half-built outputs - object code and symbols - so a failed assemble hands back nothing
// to consume; only the diagnostics remain. Returns 0, the failure signal the entry points hand back.
static uint32_t assemble_failed(baron *b)
{
    sections_reset(&b->sections);   // a fresh, empty default section - so a failed read hands back no code
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
            // Layout has settled: now assign real zero-page bytes to the ZPAUTO variables and patch the
            // placeholder operands. This refuses (records errors, patches nothing) on anything it cannot prove
            // correct, so a fresh error here fails the assemble just like a pass error would.
            zeropage_finalize(b, scratch);
            if (baron_has_errors(b)) {
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

// Turn a finished baron into the read-only snapshot the caller keeps. The section list (each section's pc +
// object code) lives in the per_pass arena from the final pass; diagnostics in permanent. The scope tree's
// backing (nodes + trie pools) is in permanent too. Both are handed back as cheap views/projections that
// outlive `b` (which dies the moment we return) - nothing is flattened up front; the caller queries on demand.
static baron_result baron_result_make(baron *b, uint32_t passes)
{
    return (baron_result) {
        .passes      = passes,
        .sections    = sections_all(&b->sections),
        .diagnostics = b->diagnostics.view,
        .scopes      = scopes_view_make(&b->scopes),
    };
}

rc_view_bytes baron_result_code(const baron_result *r)
{
    RC_ASSERT(r != NULL);
    if (r->sections.num == 0) {
        return (rc_view_bytes) {0};
    }
    return rc_view_section_get(r->sections, sections_default).code.view;
}

value baron_result_symbol(const baron_result *r, rc_str path)
{
    RC_ASSERT(r != NULL);
    return scopes_view_get_symbol(r->scopes, path);
}

baron_result assemble_string(baron_arenas *arenas, rc_str name, rc_str text)
{
    RC_ASSERT(arenas != NULL);
    baron b = baron_make(arenas);
    uint32_t source = source_files_add_string(&b.source_files, name, text);
    return baron_result_make(&b, run_passes(&b, source, arenas->scratch));
}

baron_result assemble_file(baron_arenas *arenas, rc_str path)
{
    RC_ASSERT(arenas != NULL);
    baron b = baron_make(arenas);
    uint32_t source = source_files_add_file(&b.source_files, path);
    if (source == RC_INDEX_NONE) {
        baron_error(&b, error_type_source_load, (cursor) {0});
        return baron_result_make(&b, assemble_failed(&b));   // assemble_failed clears outputs and returns 0
    }
    return baron_result_make(&b, run_passes(&b, source, arenas->scratch));
}




#ifdef BARON_TESTS

#include "richc/test.h"
#include "richc/mstr.h"   // the stress test builds a big source with rc_mstr

RC_TEST_GROUP_DATA(assemble) {
    baron_arenas arenas;
    baron_result r;      // the last assemble's snapshot; the helpers below read it back
};

RC_TEST_GROUP_INIT(assemble, fix)
{
    fix->arenas = baron_arenas_make();
}

RC_TEST_GROUP_DEINIT(assemble, fix)
{
    baron_arenas_deinit(&fix->arenas);
}

// Assemble a snippet, stash its result in fix->r, and yield the pass count (0 on failure). Each call is an
// independent assemble on the shared arenas, so distinct snippets need no distinct names. Object code,
// symbols and diagnostics are then read back from fix->r via the helpers.
#define ASM(src) (fix->r = assemble_string(&fix->arenas, RC_STR(src), RC_STR(src)), fix->r.passes)

// Whether a clean assemble (passes != 0) laid down exactly these bytes. `passes` is taken explicitly so a
// call can wrap ASM directly - code_is(&fix->r, ASM(src), exp, n) - reading the freshly stashed result.
static bool code_is(const baron_result *r, uint32_t passes, const uint8_t *exp, uint32_t n)
{
    rc_view_bytes code = baron_result_code(r);
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

// Like code_is but for a named section by index - used when a test puts its code in a SECTION (rather than
// the default at index 0) to exercise an explicit org address.
static bool code_in_is(const baron_result *r, uint32_t passes, uint32_t index, const uint8_t *exp, uint32_t n)
{
    if (passes == 0 || index >= r->sections.num) {
        return false;
    }
    rc_view_bytes code = rc_view_section_get(r->sections, index).code.view;
    if (code.num != n) {
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
static error_type first_error(const baron_result *r)
{
    for (uint32_t i = 0; i < r->diagnostics.num; i++) {
        diagnostic d = rc_view_diagnostic_get(r->diagnostics, i);
        if (d.severity == severity_error) {
            return d.code;
        }
    }
    return error_type_none;
}

// Assemble and yield that first error code - the common "what error did this snippet raise?" check.
#define ERR(src) ((void)ASM(src), first_error(&fix->r))

// Whether any recorded diagnostic - of any severity - carries this code.
static bool has_diag(const baron_result *r, error_type code)
{
    for (uint32_t i = 0; i < r->diagnostics.num; i++) {
        if (rc_view_diagnostic_get(r->diagnostics, i).code == code) {
            return true;
        }
    }
    return false;
}

RC_TEST_STEP(assemble, addressing_modes, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #0"),       (uint8_t[]) {0xA9, 0x00}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA &70"),      (uint8_t[]) {0xA5, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA &2000"),    (uint8_t[]) {0xAD, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA &70,X"),    (uint8_t[]) {0xB5, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA &2000,X"),  (uint8_t[]) {0xBD, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDX &70,Y"),    (uint8_t[]) {0xB6, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA (&70),Y"),  (uint8_t[]) {0xB1, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("STA (&70,X)"),  (uint8_t[]) {0x81, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("JMP (&1234)"),  (uint8_t[]) {0x6C, 0x34, 0x12}, 3));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("JMP &70"),      (uint8_t[]) {0x4C, 0x70, 0x00}, 3));   // no zp JMP
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ASL"),          (uint8_t[]) {0x0A}, 1));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ASL A"),        (uint8_t[]) {0x0A}, 1));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("NOP"),          (uint8_t[]) {0xEA}, 1));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("STA &70"),      (uint8_t[]) {0x85, 0x70}, 2));

    // Register letters are only registers where the grammar expects one. A symbol named after a register
    // reads as that symbol in the operand base: `x`/`y`/`a` bound to &70 give a zero-page access, not a
    // register. (`a` is legal as a plain symbol - only ZPAUTO forbids it.)
    RC_CHECK_TRUE(code_is(&fix->r, ASM("x = &70 : STA x"),  (uint8_t[]) {0x85, 0x70}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("y = &71 : LDA y"),  (uint8_t[]) {0xA5, 0x71}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("a = &72 : LDA a"),  (uint8_t[]) {0xA5, 0x72}, 2));
    // But `ASL A` is accumulator mode FIRST, even when a symbol `a` is in scope - the register wins at the
    // accumulator slot. To shift memory at `a` you would address it another way (e.g. via a differently named
    // label); the ambiguity resolves in the accumulator's favour by design.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("a = &72 : ASL A"),  (uint8_t[]) {0x0A}, 1));
    // And the comma position keeps its register meaning: `x` as an index is the X register, not the symbol.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("x = &99 : LDA &70,X"), (uint8_t[]) {0xB5, 0x70}, 2));
}

RC_TEST_STEP(assemble, multiple_statements, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #1 : STA &70 : RTS"),
                          (uint8_t[]){0xA9, 0x01, 0x85, 0x70, 0x60}, 5));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #1\nSTA &70\nRTS"),
                          (uint8_t[]){0xA9, 0x01, 0x85, 0x70, 0x60}, 5));
}

RC_TEST_STEP(assemble, implied_opcode_before_close_brace, fix)
{
    // Regression: an implied / accumulator opcode immediately before '}' (no separator) is a no-operand
    // statement, not an attempt to read '}' as an operand. (`.routine { RTS }` is about the commonest 6502
    // shape there is; it used to raise a spurious expression error.)
    RC_CHECK_TRUE(code_is(&fix->r, ASM(".routine { RTS }"),      (uint8_t[]){0x60}, 1));         // named scope
    RC_CHECK_TRUE(code_is(&fix->r, ASM("{ NOP }"),               (uint8_t[]){0xEA}, 1));         // anon scope
    RC_CHECK_TRUE(code_is(&fix->r, ASM(".s { ASL }"),            (uint8_t[]){0x0A}, 1));          // accumulator
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDX #2 : .l { DEX }"),   (uint8_t[]){0xA2, 0x02, 0xCA}, 3));
    // A '}' still cannot masquerade as an operand for an instruction that requires one.
    RC_CHECK_TRUE(ERR("{ LDA }") == error_type_missing_operand);
}

RC_TEST_STEP(assemble, branch_offsets, fix)
{
    // Backward: target at pc 0, NOP, then BNE back to it. offset = 0 - (1 + 2) = -3 = 0xFD.
    RC_CHECK_TRUE(code_is(&fix->r, ASM(".t NOP : BNE t"), (uint8_t[]){0xEA, 0xD0, 0xFD}, 3));
    // Forward: BEQ over a following NOP. BEQ at 0, NOP at 2, target = 3, offset = 3 - 2 = 1.
    // (`over` not `skip`: SKIP is now a reserved directive keyword, like a mnemonic.)
    RC_CHECK_TRUE(code_is(&fix->r, ASM("BEQ over : NOP : .over"), (uint8_t[]){0xF0, 0x01, 0xEA}, 3));
}

RC_TEST_STEP(assemble, branch_to_shadowed_forward_label, fix)
{
    // `.label` names the scope, and an inner `.label` shadows it. On the first pass the inner label
    // is not yet bound, so BEQ transiently resolves to the FAR outer label (256+ bytes back) - a
    // branch that would be out of range. That error must be withheld on non-final passes; by the
    // time the layout settles the near inner label is bound and the branch is in range (offset 0).
    uint32_t passes = ASM(".label { SKIP 256 : BEQ label : .label }");
    RC_CHECK_TRUE(passes != 0);
    rc_view_bytes code = baron_result_code(&fix->r);
    RC_CHECK(code.num, ==, 258u);                                  // 256 skipped + BEQ (2 bytes)
    RC_CHECK((uint32_t)rc_view_bytes_get(code, 256), ==, 0xF0u);   // BEQ opcode
    RC_CHECK((uint32_t)rc_view_bytes_get(code, 257), ==, 0x00u);   // resolves near: branch to the next byte
}

RC_TEST_STEP(assemble, skip_skipto_align, fix)
{
    // SKIP n emits n zero bytes.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #1 : SKIP 3 : RTS"), (uint8_t[]) {0xA9, 0x01, 0x00, 0x00, 0x00, 0x60}, 6));

    // SKIPTO addr fills with zeroes until pc reaches addr (pc 1 -> 4 = three bytes; default section, org 0).
    RC_CHECK_TRUE(code_is(&fix->r, ASM("NOP : SKIPTO 4 : RTS"), (uint8_t[]) {0xEA, 0x00, 0x00, 0x00, 0x60}, 5));
    // Already past addr is an error.
    RC_CHECK_TRUE(ERR("NOP : NOP : SKIPTO 1") == error_type_skip_backwards);

    // ALIGN n pads up to the next multiple of n (pc 1 -> 4 = three bytes)...
    RC_CHECK_TRUE(code_is(&fix->r, ASM("NOP : ALIGN 4 : RTS"), (uint8_t[]) {0xEA, 0x00, 0x00, 0x00, 0x60}, 5));
    // ...and is a no-op when pc already sits on the boundary.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ALIGN 4 : NOP"), (uint8_t[]) {0xEA}, 1));
    // ALIGN 0 is meaningless.
    RC_CHECK_TRUE(ERR("ALIGN 0") == error_type_bad_alignment);
}

RC_TEST_STEP(assemble, skip_advances_pc, fix)
{
    // SKIP moves pc, so a label after it sees the advanced address (default section, org 0).
    uint32_t passes = ASM(".a SKIP 4 : .b");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("a")), value_make_numeric(0)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("b")), value_make_numeric(4)));
}

RC_TEST_STEP(assemble, section_selects_and_creates, fix)
{
    // A SECTION block emits into a named section, not the default. The default (index 0) stays empty;
    // "code" (index 1) carries the two instructions. ENDSECTION returns to the default.
    RC_CHECK_TRUE(ASM("SECTION code : LDA #&11 : LDX #&22 : ENDSECTION") != 0);
    RC_CHECK(fix->r.sections.num, ==, 2u);   // default + "code"
    section def  = rc_view_section_get(fix->r.sections, 0);
    section code = rc_view_section_get(fix->r.sections, 1);
    RC_CHECK(def.code.view.num, ==, 0u);     // nothing landed in the default
    RC_CHECK(code.code.view.num, ==, 4u);    // LDA #, LDX # -> 4 bytes
    RC_CHECK_TRUE(rc_str_is_equal(code.name, RC_STR("code")));
}

RC_TEST_STEP(assemble, section_names_are_unique, fix)
{
    // Distinct sections each hold their own bytes; there is no concatenation, so a repeated name is refused.
    RC_CHECK_TRUE(ASM("SECTION a : EQUB 1 : ENDSECTION : SECTION b : EQUB 2 : ENDSECTION") != 0);
    RC_CHECK(fix->r.sections.num, ==, 3u);   // default + a + b
    RC_CHECK(rc_view_section_get(fix->r.sections, 1).code.view.num, ==, 1u);   // a got one byte
    RC_CHECK(rc_view_section_get(fix->r.sections, 2).code.view.num, ==, 1u);   // b got one
    RC_CHECK_TRUE(ERR("SECTION a : ENDSECTION : SECTION a : ENDSECTION") == error_type_duplicate_section);
}

RC_TEST_STEP(assemble, section_org_attribute, fix)
{
    // `org` on the SECTION line sets that section's start address; a label inside takes it, and the section's
    // pc advances past the emitted byte.
    RC_CHECK_TRUE(ASM("SECTION hi, org = &3000 : .here EQUB 0 : ENDSECTION") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(0x3000)));
    RC_CHECK(rc_view_section_get(fix->r.sections, 1).pc, ==, 0x3001u);   // &3000 + 1 emitted byte
}

RC_TEST_STEP(assemble, section_cursor_continues, fix)
{
    // A section with no `org` continues the running cursor: the first section runs at &2000 and emits one
    // byte, so the second (no org) begins at &2001. Labels confirm both.
    RC_CHECK_TRUE(ASM("SECTION a, org=&2000 : .x EQUB 0 : ENDSECTION : SECTION b : .y EQUB 0 : ENDSECTION") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("x")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("y")), value_make_numeric(0x2001)));
}

RC_TEST_STEP(assemble, section_nesting_inherits_attributes, fix)
{
    // A nested section inherits its parent's attributes (its own keys override); `org` never inherits. Here
    // the child inherits load=&1200 and overrides tag; a stored attribute round-trips into the result.
    RC_CHECK_TRUE(ASM("SECTION outer, load=&1200, tag=1 : SECTION inner, tag=2 : EQUB 0 : ENDSECTION : ENDSECTION") != 0);
    section inner = rc_view_section_get(fix->r.sections, 2);   // default, outer, inner
    RC_CHECK_TRUE(rc_str_is_equal(inner.name, RC_STR("inner")));
    // inner has load (inherited), tag (overridden to 2), and no org.
    bool saw_load = false, saw_tag = false;
    for (uint32_t i = 0; i < inner.attributes.view.num; i++) {
        attribute a = rc_view_attribute_get(inner.attributes.view, i);
        if (rc_str_is_equal(a.key, RC_STR("load"))) { saw_load = true; RC_CHECK(a.v.numeric, ==, 4608.0); }   // &1200 inherited
        if (rc_str_is_equal(a.key, RC_STR("tag")))  { saw_tag  = true; RC_CHECK(a.v.numeric, ==, 2.0); }       // own override
    }
    RC_CHECK_TRUE(saw_load);
    RC_CHECK_TRUE(saw_tag);
}

RC_TEST_STEP(assemble, section_name_errors, fix)
{
    RC_CHECK_TRUE(ERR("SECTION")   == error_type_expected_section_name);   // nothing after the keyword
    RC_CHECK_TRUE(ERR("SECTION 5 : ENDSECTION") == error_type_expected_section_name);   // a number is not a name
    RC_CHECK_TRUE(ERR("SECTION a : LDA #0") == error_type_unclosed_section);   // no ENDSECTION
    RC_CHECK_TRUE(ERR("ENDSECTION") == error_type_unexpected_endsection);   // no SECTION to close
}

RC_TEST_STEP(assemble, zpreserve_directive, fix)
{
    // A plain range, a comma-list of ranges and a bare byte all parse and assemble cleanly (no code).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&8F") != 0);
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F, &A0..&A7") != 0);
    RC_CHECK_TRUE(ASM("ZPRESERVE &70") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);

    // An empty exclusive range is a legal empty list: it enables the feature but reserves nothing.
    RC_CHECK_TRUE(ASM("ZPRESERVE 5..<5") != 0);

    // The address may be forward-referenced; it defers and settles on a later pass (still converges).
    RC_CHECK_TRUE(ASM("ZPRESERVE base..base+3 : base = &70") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, zpreserve_errors, fix)
{
    // Out of the zero page (high, and via a range that overshoots) - a recoverable semantic error.
    RC_CHECK_TRUE(ERR("ZPRESERVE &100")      == error_type_reserve_not_zeropage);
    RC_CHECK_TRUE(ERR("ZPRESERVE &FE..&102") == error_type_reserve_not_zeropage);
    // A non-numeric operand is not an address.
    RC_CHECK_TRUE(ERR("ZPRESERVE \"hi\"")    == error_type_operand_not_numeric);
}

RC_TEST_STEP(assemble, zpauto_declares_scoped_var, fix)
{
    // ZPAUTO1/ZPAUTO2 bind scoped symbols; after convergence the allocator gives each a real zero-page byte.
    // Neither variable is ever referenced, so there is no liveness to prove sharing safe - each keeps its own
    // byte. FFD places the 2-byte `ptr` first (&70-&71), then the 1-byte `foo` at the next free byte, &72.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 foo : ZPAUTO2 ptr");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("ptr")), value_make_numeric(0x70)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("foo")), value_make_numeric(0x72)));

    // A comma-list declares several at once.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 p, q, r") != 0);
    RC_CHECK_FALSE(value_is_none(baron_result_symbol(&fix->r, RC_STR("p"))));
    RC_CHECK_FALSE(value_is_none(baron_result_symbol(&fix->r, RC_STR("r"))));

    // Declared inside a named routine, a var is reachable from outside as routine.name (a dotted path).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : .routine { ZPAUTO1 v : RTS }") != 0);
    RC_CHECK_FALSE(value_is_none(baron_result_symbol(&fix->r, RC_STR("routine.v"))));
}

RC_TEST_STEP(assemble, zpauto_errors, fix)
{
    // A variable with no ZPRESERVE to allocate from is flagged (but still bound, to avoid a cascade).
    RC_CHECK_TRUE(ERR("ZPAUTO1 foo") == error_type_var_without_reserve);
    // A missing or dotted name.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1")     == error_type_expected_var_name);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 a.b") == error_type_expected_var_name);
    // Re-declaring the same name in one scope is a duplicate (mirrors labels).
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 foo : ZPAUTO1 foo") == error_type_duplicate_symbol);
    // `a` alone is rejected (ambiguous with accumulator addressing, ASL A). X and Y are fine now - they only
    // read as registers after a comma - so a variable may be named x or y.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 A")      == error_type_zpauto_register_name);
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 x") != 0);
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 Y") != 0);
}

RC_TEST_STEP(assemble, zpauto_operand_is_zeropage, fix)
{
    // A ZPAUTO variable in operand position assembles as a 2-byte zero-page access. Post-convergence the
    // allocator assigns the real byte and patches the operand in place - it never changes size (the layout-
    // independence property), so patching is sound. With one variable and a free block from &70, it lands on
    // the first reserved byte, &70; a 2-byte pointer's hi half is &71 (its low byte + 1).
    const uint8_t b0 = 0x70;   // the first reserved byte the single variable is allocated to

    // Declared then used: LDA zp / STA zp (read and write), operand patched to the allocated address.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ZPRESERVE &70..&7F : ZPAUTO1 foo : LDA foo"),
                          (uint8_t[]){0xA5, b0}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ZPRESERVE &70..&7F : ZPAUTO1 foo : STA foo"),
                          (uint8_t[]){0x85, b0}, 2));

    // Used BEFORE declared: still 2 bytes, still patched. It sizes as zero-page every pass (a VAR is always
    // ZP), so the layout converges no matter the eventual address - the binding from one pass resolves the next.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ZPRESERVE &70..&7F : LDA foo : ZPAUTO1 foo"),
                          (uint8_t[]){0xA5, b0}, 2));

    // A 2-byte pointer: lo is `ptr` (&70), hi is `ptr+1` (&71 - the base plus the intra-variable offset that
    // the operand carried); and the pointer drives indirect-indexed addressing (its intended use).
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : LDA ptr : LDA ptr+1"),
                          (uint8_t[]){0xA5, b0, 0xA5, (uint8_t)(b0 + 1)}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : LDA (ptr),Y"),
                          (uint8_t[]){0xB1, b0}, 2));

    // A variable named `x` (or `y`) now reads as a symbol in the operand base - the register-aware table is
    // only consulted after a comma - so it attributes and is patched to its allocated byte like any other.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("ZPRESERVE &70..&7F : ZPAUTO1 x : STA x : LDA x"),
                          (uint8_t[]){0x85, b0, 0xA5, b0}, 4));
}

// A ZPAUTO symbol's allocated zero-page address, or -1 if it is not a plain number (unbound / failed).
static int64_t zp_addr(const baron_result *r, const char *name)
{
    value v = baron_result_symbol(r, rc_str_from_cstr(name));
    return value_is_numeric(v) ? (int64_t) v.numeric : -1;
}

RC_TEST_STEP(assemble, zpauto_allocates_and_reuses, fix)
{
    // D1 - two locals with disjoint live ranges share one byte. v1 dies (last read) before v2 is written, so
    // the colourer packs both onto &70.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v1, v2 : STA v1 : LDA v1 : STA v2 : LDA v2 : RTS") != 0);
    RC_CHECK(zp_addr(&fix->r, "v1"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "v2"), ==, 0x70);

    // D2 - the spec's mul shape: in1 is read first (input), tmp is written-then-read (temp), out1 is written
    // last (output). in1 and tmp overlap so tmp takes &71; out1 is born only after both die, so it REUSES
    // in1's &70. Six accesses collapse onto two bytes.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 in1, tmp, out1\n"
                      "LDA in1 : ASL A : STA tmp : LDA in1 : CLC : ADC tmp : STA out1 : RTS") != 0);
    RC_CHECK(zp_addr(&fix->r, "in1"),  ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "tmp"),  ==, 0x71);
    RC_CHECK(zp_addr(&fix->r, "out1"), ==, 0x70);   // reuses in1's byte

    // D3 - a 2-byte pointer packs beside a 1-byte temp when they interfere. `a` is live across the pointer's
    // setup, so it cannot overlap ptr's two bytes (&70-&71) and lands at &72.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : ZPAUTO1 t\n"
                      "STA t : STA ptr : STA ptr+1 : LDA t : LDA (ptr),Y : RTS") != 0);
    RC_CHECK(zp_addr(&fix->r, "ptr"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "t"),   ==, 0x72);
}

RC_TEST_STEP(assemble, zpauto_call_without_live_var_is_allowed, fix)
{
    // A JSR is fine as long as no ZPAUTO variable is live across it: `t` is written and read BEFORE the call,
    // dead by the time control leaves, so the callee cannot clobber it. Allocation proceeds normally.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 t : STA t : LDA t : JSR sub : RTS : .sub { RTS }");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "t"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_interprocedural_allocation, fix)
{
    // The interprocedural core: `keep` is live across a JSR to `sub`, which has its own local `loc`. The call
    // clobbers sub's footprint, so keep must NOT share loc's byte - it interferes with the whole callee
    // footprint and lands on a different byte. (Before this rule, both took &70 and the call would corrupt
    // keep.) keep is placed first at &70, loc is pushed to &71.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                          "LDA #1 : STA keep : JSR sub : LDA keep : RTS\n"
                          ".sub { ZPAUTO1 loc : STA loc : LDA loc : RTS }");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"),    ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "sub.loc"), ==, 0x71);   // forced off keep's byte by the call

    // Contrast: a var DEAD across the call does not interfere with the callee, so it may reuse the byte. Here
    // `tmp` dies before the call, so it can share sub.loc's byte.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 tmp\n"
                      "STA tmp : LDA tmp : JSR sub : RTS\n"
                      ".sub { ZPAUTO1 loc : STA loc : LDA loc : RTS }") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "tmp"),     ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "sub.loc"), ==, 0x70);   // dead across the call -> reuses the callee's byte
}

RC_TEST_STEP(assemble, zpauto_allocation_refusals, fix)
{
    // The certainty contract: rather than emit code it cannot prove correct, the allocator errors and points
    // at the fix. Each of these MUST fail the assemble.

    // A variable live across a RECURSIVE call cannot live in one static byte (each level needs its own).
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : .r { ZPAUTO1 cnt : STA cnt : JSR r : LDA cnt : RTS }")
                  == error_type_zpauto_recursion);

    // A computed / indirect jump reaches code the CFG cannot follow while a variable is in play - refuse and
    // ask for an annotation.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : JMP (&2000)")
                  == error_type_zpauto_computed_flow);

    // More simultaneously-live variables than reserved bytes is a spill: p and q overlap but only &70 is free.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 p, q : STA p : STA q : LDA p : LDA q : RTS")
                  == error_type_zeropage_full);
}

RC_TEST_STEP(assemble, zpauto_indexed_access_is_refused, fix)
{
    // The direct-addressing envelope: an auto-variable reached by an indexed mode touches var+index - a byte
    // the allocator cannot account for and may have placed another variable in. It is refused rather than
    // silently miscompiled. `v,X` (zero-page indexed), `v,Y` (widens to absolute indexed, no zp form), and
    // `(p,X)` (indexed-indirect) are all outside the envelope.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : LDA v,X") == error_type_zpauto_indexed_access);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : LDA v,Y") == error_type_zpauto_indexed_access);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO2 p : LDA (p,X)") == error_type_zpauto_indexed_access);

    // The envelope-safe forms stay legal: direct `var`, the `var+1` hi byte, and the whole-pointer `(var),Y`
    // dereference (the intended ZPAUTO2 use - only the DATA is indexed by Y, the pointer itself is read direct).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 p : STA p : STA p+1 : LDA (p),Y") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, zpauto_layout_must_not_collide, fix)
{
    // The flow analysis identifies a block by its address, so the ONLY rule is that no two instructions share
    // an address. Distinct sections at DIFFERENT addresses are free.

    // A section with an explicit org is fine - the var still allocates.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : SECTION s, org=&2000 : STA v : LDA v : RTS : ENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);

    // Two sections at DIFFERENT addresses are fine, and their variables reuse the same reserved byte (no flow
    // connects them, so they do not interfere).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v, w\n"
                      "SECTION a, org=&2000 : STA v : LDA v : RTS : ENDSECTION\n"
                      "SECTION b, org=&3000 : STA w : LDA w : RTS : ENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "w"), ==, 0x70);

    // But two sections loaded into the SAME address slot put two instructions at one address - refused rather
    // than risk a wrong edge, and named as a section clash.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      "SECTION a, org=&2000 : STA v : ENDSECTION\n"
                      "SECTION b, org=&2000 : LDA v : RTS : ENDSECTION")
                  == error_type_zpauto_multi_section);

    // And a nested section whose low org threads the cursor BACK onto emitted code collides within one section
    // (STA v at pc 0, then an empty org=0 section drags the default cursor back to 0, so LDA v lands on it) -
    // named as an org rewind.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : SECTION a, org=0 : ENDSECTION : LDA v : RTS")
                  == error_type_zpauto_org_rewind);
}

RC_TEST_STEP(assemble, zpauto_unreachable_prunes_dead_edge, fix)
{
    // UNREACHABLE tells the allocator an always-taken branch cannot fall through. `hot` (used on the taken
    // path) and `cold` (used only on the never-taken fall-through) would otherwise both be live across the
    // branch and interfere - two variables, but only &70 reserved, so a spill.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 hot, cold\n"
                      "STA hot : BNE taken : LDA cold : RTS\n"
                      ".taken\n"
                      "LDA hot : RTS") == error_type_zeropage_full);

    // With UNREACHABLE after the branch, the dead fall-through edge is pruned: cold is no longer live across
    // the branch, hot and cold no longer interfere, and they share the single reserved byte.
    uint32_t passes = ASM("ZPRESERVE &70 : ZPAUTO1 hot, cold\n"
                          "STA hot : BNE taken : UNREACHABLE : LDA cold : RTS\n"
                          ".taken\n"
                          "LDA hot : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "hot"),  ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "cold"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_multi_entry_multi_exit, fix)
{
    // A single braced block with TWO entry points (each its own JSR target) and several RTS exits, including
    // the BCC over : RTS : .over early-out shape. Routines are recovered from the CFG, so this is not one
    // routine with one entry - it is two, and both allocate cleanly. v (entry1) and w (entry2) never overlap,
    // so they share &70.
    uint32_t p = ASM("ZPRESERVE &70..&7F\n"
                     "JSR routine.entry1 : JSR routine.entry2 : RTS\n"
                     ".routine {\n"
                     "  .entry1 : ZPAUTO1 v : STA v : LDA v : RTS\n"
                     "  .entry2 : ZPAUTO1 w : STA w : BCC over : RTS\n"
                     "  .over : LDA w : RTS\n"
                     "}");
    RC_CHECK_TRUE(p != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "routine.v"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "routine.w"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_control_flow_crosses_scopes, fix)
{
    // Scopes name; the CFG is recovered from real branches, not braces. Here `n` is written inside .work and is
    // live across a BEQ that leaves the block for the shared .done label in another scope (referenced there by
    // its dotted path - naming IS by scope). Liveness follows the edge across the brace, so the allocation is
    // still correct and `n` settles on &70. A statically-known target may cross a scope boundary freely.
    uint32_t p = ASM("ZPRESERVE &70..&7F\n"
                     ".work { ZPAUTO1 n : STA n : BEQ done : LDA n : STA n }\n"
                     ".done : LDA work.n : RTS");
    RC_CHECK_TRUE(p != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "work.n"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_cancall_bounds_dispatched_call, fix)
{
    // Baseline: the JSR is taken at its literal target (handlerA) alone, so `keep` (live across the call)
    // interferes only with handlerA's footprint. handlerB is never called, so its local is a free agent and
    // reuses keep's byte.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                      "STA keep : JSR handlerA : LDA keep : RTS\n"
                      ".handlerA { ZPAUTO1 ha : STA ha : LDA ha : RTS }\n"
                      ".handlerB { ZPAUTO1 hb : STA hb : LDA hb : RTS }") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"),        ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "handlerB.hb"), ==, 0x70);   // unseen callee -> reuses keep's byte

    // CANCALL declares the call may reach handlerA OR handlerB (a self-modified / dispatched JSR). Now its
    // footprint covers both, keep interferes with handlerB.hb too, and hb is forced off keep's byte.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                          "STA keep : JSR handlerA : CANCALL handlerA, handlerB : LDA keep : RTS\n"
                          ".handlerA { ZPAUTO1 ha : STA ha : LDA ha : RTS }\n"
                          ".handlerB { ZPAUTO1 hb : STA hb : LDA hb : RTS }");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"),        ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "handlerB.hb"), !=, 0x70);   // now inside the call footprint
}

RC_TEST_STEP(assemble, zpauto_canjump_bounds_computed_jump, fix)
{
    // A computed / indirect JMP with a variable live across it is refused: the CFG cannot see where control
    // goes, so it cannot prove `keep` survives.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 keep : vector = &2000\n"
                      "STA keep : JMP (vector)\n"
                      ".hA : LDA keep : RTS\n"
                      ".hB : LDA keep : RTS") == error_type_zpauto_computed_flow);

    // CANJUMP declares the jump table's targets, so the CFG wires each as a real successor edge: keep is live
    // into both arms and allocates cleanly onto &70.
    uint32_t p = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep : vector = &2000\n"
                     "STA keep : JMP (vector) : CANJUMP hA, hB\n"
                     ".hA : LDA keep : RTS\n"
                     ".hB : LDA keep : RTS");
    RC_CHECK_TRUE(p != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_macro_local_is_per_invocation, fix)
{
    // A macro can declare its own ZPAUTO temp without caring where it lands. Each invocation runs in its own
    // child scope, so each `loc` is a DISTINCT variable (identity = scope + def), exactly like two sibling
    // blocks declaring the same name. Invoked twice, the two disjoint locals both settle on &70.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : MACRO USE : ZPAUTO1 loc : STA loc : LDA loc : ENDMACRO\n"
                          "USE\nUSE");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0x85, 0x70, 0xA5, 0x70, 0x85, 0x70, 0xA5, 0x70}, 8));

    // Likewise inside a FOR body that iterates more than once: each iteration's temp is its own variable.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : FOR i = 0..1 : ZPAUTO1 loc : STA loc : LDA loc : NEXT") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST(assemble, zpauto_rw_observation)
{
    // The final pass records each ZPAUTO-touching instruction into the ZP IR with its read/write class,
    // attributed to the right vreg. The IR is internal (not on baron_result yet), so we build a baron
    // directly and read its zeropage. [Stage B1]
    baron_arenas arenas = baron_arenas_make();

    // Straight-line: STA is a write, LDA a read, INC a read-modify-write; all name the one variable (vreg 0).
    {
        baron b = baron_make(&arenas);
        uint32_t s = source_files_add_string(&b.source_files, RC_STR("t1"),
            RC_STR("ZPRESERVE &70..&7F : ZPAUTO1 foo : STA foo : LDA foo : INC foo"));
        run_passes(&b, s, arenas.scratch);
        RC_CHECK(zeropage_insn_count(&b.zeropage), ==, 3u);
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 0).rw, ==, (int) vref_write);
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 1).rw, ==, (int) vref_read);
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 2).rw, ==, (int) (vref_read | vref_write));
        RC_CHECK(zeropage_insn_get(&b.zeropage, 0).vreg, ==, 0u);
        RC_CHECK(zeropage_insn_get(&b.zeropage, 1).vreg, ==, 0u);
        RC_CHECK(zeropage_insn_get(&b.zeropage, 2).vreg, ==, 0u);
    }

    // A 2-byte pointer: lo (ptr) and hi (ptr+1) both attribute to the one vreg; an indirect (ptr),Y READS
    // the pointer to dereference it - even a store THROUGH it reads the pointer (the write hits the target).
    {
        baron b = baron_make(&arenas);
        uint32_t s = source_files_add_string(&b.source_files, RC_STR("t2"),
            RC_STR("ZPRESERVE &70..&7F : ZPAUTO2 ptr : STA ptr : STA ptr+1 : LDA (ptr),Y : STA (ptr),Y"));
        run_passes(&b, s, arenas.scratch);
        RC_CHECK(zeropage_insn_count(&b.zeropage), ==, 4u);
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 0).rw, ==, (int) vref_write);   // STA ptr
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 1).rw, ==, (int) vref_write);   // STA ptr+1
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 2).rw, ==, (int) vref_read);    // LDA (ptr),Y
        RC_CHECK((int) zeropage_insn_get(&b.zeropage, 3).rw, ==, (int) vref_read);    // STA (ptr),Y
        for (uint32_t i = 0; i < 4; i++) {
            RC_CHECK(zeropage_insn_get(&b.zeropage, i).vreg, ==, 0u);
        }
    }

    // Every instruction is recorded (the CFG needs the whole stream), but an ordinary symbol or a literal
    // address in operand position attributes to NO vreg - only ZPAUTO operands do.
    {
        baron b = baron_make(&arenas);
        uint32_t s = source_files_add_string(&b.source_files, RC_STR("t3"),
            RC_STR("ZPRESERVE &70..&7F : label = &50 : LDA label : LDA &2000 : LDA #7"));
        run_passes(&b, s, arenas.scratch);
        RC_CHECK(zeropage_insn_count(&b.zeropage), ==, 3u);   // all three LDAs are recorded...
        for (uint32_t i = 0; i < 3; i++) {
            RC_CHECK(zeropage_insn_get(&b.zeropage, i).vreg, ==, RC_INDEX_NONE);   // ...but touch no vreg
        }
    }

    baron_arenas_deinit(&arenas);
}

RC_TEST(assemble, zpauto_liveness_end_to_end)
{
    // The whole Stage C chain over REAL assembler output: parse the spec's `mul` routine, take the recorded
    // IR straight off b.zeropage, build the CFG and run liveness, and confirm the two facts the analysis
    // exists to establish - the in/out/temp classification, and the byte-reuse (out1 reuses in1's / tmp's
    // range because their live ranges are disjoint). vreg ids follow declaration order: in1=0, tmp=1, out1=2.
    baron_arenas arenas = baron_arenas_make();
    rc_arena arena = rc_arena_make_default();     // holds the cfg + liveness result
    rc_arena scratch = rc_arena_make_default();   // distinct by-value scratch for the analysis

    baron b = baron_make(&arenas);
    uint32_t s = source_files_add_string(&b.source_files, RC_STR("mul"),
        RC_STR("ZPRESERVE &70..&7F : ZPAUTO1 in1, tmp, out1\n"
               ".mul { LDA in1 : ASL A : STA tmp : LDA in1 : CLC : ADC tmp : STA out1 : RTS }"));
    run_passes(&b, s, arenas.scratch);

    cfg g = cfg_build(zeropage_insns(&b.zeropage), zeropage_cflows(&b.zeropage), &arena, scratch);
    liveness lv = liveness_analyze(g, zeropage_insns(&b.zeropage), zeropage_var_count(&b.zeropage), 0,
                                   &arena, scratch);

    // Classification: in1 is read before written (input), tmp is born and consumed inside (temp), out1 is
    // written but never read inside (output).
    RC_CHECK_TRUE(liveness_class_of(&lv, 0) == vreg_class_input);
    RC_CHECK_TRUE(liveness_class_of(&lv, 1) == vreg_class_temp);
    RC_CHECK_TRUE(liveness_class_of(&lv, 2) == vreg_class_output);

    // Reuse: in1 and tmp overlap (interfere), but out1 is born only after both die, so it interferes with
    // neither - it can share a byte with either.
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));    // in1 - tmp
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 2));    // in1 - out1
    RC_CHECK_FALSE(liveness_interferes(&lv, 1, 2));    // tmp - out1

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
    baron_arenas_deinit(&arenas);
}

RC_TEST_STEP(assemble, org_and_labels, fix)
{
    // A section's `org` sets label values but not where code lands (code still fills its buffer from index 0).
    uint32_t passes = ASM("SECTION t, org=&2000 : LDA #1 : .here : ENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(code_in_is(&fix->r, passes, 1, (uint8_t[]){0xA9, 0x01}, 2));   // section "t" is index 1
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")),
                                 value_make_numeric(0x2002)));
}

RC_TEST_STEP(assemble, local_label_backward, fix)
{
    // .@ marks a spot; @- is the nearest .@ before the reference. Same shape as branch_offsets' .t/BNE:
    // target at pc 0, NOP, then BNE back to it -> offset 0 - (1 + 2) = -3 = 0xFD.
    RC_CHECK_TRUE(code_is(&fix->r, ASM(".@ NOP : BNE @-"), (uint8_t[]){0xEA, 0xD0, 0xFD}, 3));
}

RC_TEST_STEP(assemble, local_label_forward, fix)
{
    // @+ is the nearest .@ AFTER the reference - a forward reference, so it only resolves once the later
    // .@ has bound on an earlier pass. BEQ at 0, NOP at 2, .@ at pc 3 -> offset 3 - 2 = 1.
    uint32_t passes = ASM("BEQ @+ : NOP : .@");
    RC_CHECK_TRUE(passes >= 2);                                                   // pass 1 defers @+, a later pass resolves it
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xF0, 0x01, 0xEA}, 3));
}

RC_TEST_STEP(assemble, local_label_orders_by_cursor_not_value, fix)
{
    // Two .@ in sections at counter-to-source-order addresses (&34 then &12). @- picks the TEXTUALLY previous
    // .@ (the second one, value &12) - proving the scan orders by definition position, not by the address the
    // label captured. EQUB emits that value's low byte: 0x12, not 0x34. (Sections do not scope, so both .@
    // bind in the one enclosing scope.) The EQUB lands in section "b" (index 2).
    RC_CHECK_TRUE(code_in_is(&fix->r,
        ASM("SECTION a, org=&34 : .@ : ENDSECTION : SECTION b, org=&12 : .@ : EQUB @- : ENDSECTION"),
        2, (uint8_t[]){0x12}, 1));
}

RC_TEST_STEP(assemble, local_labels_are_scoped, fix)
{
    // Local labels do not leak across scopes: a .@ outside a brace block is invisible to @- inside it, so
    // the reference never resolves -> undefined on the final pass.
    RC_CHECK_TRUE(ERR(".@ : { EQUB @- }") == error_type_undefined_symbol);
}

RC_TEST_STEP(assemble, local_label_unmatched_is_undefined, fix)
{
    // @- with no preceding .@, and @+ with no following .@, are both genuinely unresolvable.
    RC_CHECK_TRUE(ERR("EQUB @-") == error_type_undefined_symbol);
    RC_CHECK_TRUE(ERR("EQUB @+") == error_type_undefined_symbol);
}

RC_TEST_STEP(assemble, local_label_per_for_iteration, fix)
{
    // Each FOR iteration gets its own scope, so the body's .@ is private to that iteration and @- finds it
    // there - three independent back-branches, with no duplicate-label collision across iterations.
    uint32_t passes = ASM("FOR n = 1..3 : .@ NOP : BNE @- : NEXT");
    RC_CHECK_TRUE(code_is(&fix->r, passes,
        (uint8_t[]){0xEA, 0xD0, 0xFD, 0xEA, 0xD0, 0xFD, 0xEA, 0xD0, 0xFD}, 9));
}

RC_TEST_STEP(assemble, symbol_definition, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("n = 5 : LDA #n"), (uint8_t[]){0xA9, 0x05}, 2));
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
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA5, 0x70}, 2));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, forward_reference_to_absolute, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA foo : foo = &2000"), (uint8_t[]){0xAD, 0x00, 0x20}, 3));
}

RC_TEST_STEP(assemble, named_scope_dotted_access, fix)
{
    RC_CHECK_TRUE(ASM(".routine { .core LDA #0 } : x = routine.core") != 0);   // default section, org 0
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("x")),
                                 value_make_numeric(0)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("routine.core")),
                                 value_make_numeric(0)));
}

RC_TEST_STEP(assemble, result_harvest_flattens_symbols, fix)
{
    // A result carries a read-only scopes_view; scopes_view_flatten harvests the whole spellable table on
    // demand, keyed by full dotted path (into a caller arena). The `.routine` label binds a top-level
    // `routine` AND names the child scope, so the spellable table is exactly x, routine, routine.core (all
    // at org 0); the anonymous machinery stays hidden.
    RC_CHECK_TRUE(ASM(".routine { .core LDA #0 } : x = routine.core") != 0);

    rc_arena out_arena = rc_arena_make_default();
    rc_arena scratch   = rc_arena_make_default();
    rc_view_symbol_entry syms = scopes_view_flatten(fix->r.scopes, &out_arena, scratch);

    RC_CHECK(syms.num, ==, 3u);   // exactly x, routine, routine.core
    uint32_t seen = 0;
    for (uint32_t i = 0; i < syms.num; i++) {
        symbol_entry e = rc_view_symbol_entry_get(syms, i);
        bool known = rc_str_is_equal(e.path, RC_STR("x"))
                  || rc_str_is_equal(e.path, RC_STR("routine"))
                  || rc_str_is_equal(e.path, RC_STR("routine.core"));
        RC_CHECK_TRUE(known);                                        // no stray / unspellable paths
        RC_CHECK_TRUE(value_is_equal(e.v, value_make_numeric(0)));   // every one resolves to org 0
        seen++;
    }
    RC_CHECK(seen, ==, 3u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&out_arena);
}

RC_TEST_STEP(assemble, result_exposes_sections, fix)
{
    // The result carries the whole section list, not just the default's bytes. With no SECTION that is exactly
    // one section (index 0): its code matches baron_result_code and its pc advanced past the emitted bytes.
    RC_CHECK_TRUE(ASM("LDA #&12 : LDX #&34") != 0);   // 4 bytes in the default section (org 0)
    RC_CHECK(fix->r.sections.num, ==, 1u);
    section o = rc_view_section_get(fix->r.sections, 0);
    RC_CHECK(o.code.view.num, ==, 4u);
    RC_CHECK(o.pc, ==, 4u);                                       // org 0 + 4 emitted bytes
    RC_CHECK(baron_result_code(&fix->r).num, ==, o.code.view.num);   // the convenience matches section 0
}

RC_TEST_STEP(assemble, named_scope_brace_after_separator, fix)
{
    // The naming brace may sit on the next line (or after a ':').
    RC_CHECK_TRUE(ASM(".routine\n{\n.core LDA #0\n}\nx = routine.core") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("routine.core")),
                                 value_make_numeric(0)));
}

RC_TEST_STEP(assemble, anonymous_scope_is_private, fix)
{
    RC_CHECK_TRUE(ASM("{ y = 5 }") != 0);
    RC_CHECK_TRUE(value_is_none(baron_result_symbol(&fix->r, RC_STR("y"))));
}

RC_TEST_STEP(assemble, label_does_not_own_following_statement, fix)
{
    // A label hands control straight back: a '}' may sit on the same line right after it (the label
    // no longer swallows what follows), and the inner label still binds to the current pc.
    RC_CHECK_TRUE(ASM(".r { .e }") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("r.e")), value_make_numeric(0)));
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
    uint32_t passes = (fix->r = assemble_file(&fix->arenas, RC_STR("sample.6502"))).passes;
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA9, 0x01, 0x60}, 3));

    RC_CHECK_TRUE((fix->r = assemble_file(&fix->arenas, RC_STR("no_such_file.6502")),
                   first_error(&fix->r)) == error_type_source_load);
}

RC_TEST_STEP(assemble, if_selects_branch, fix)
{
    // The active branch emits; the dead one is parsed for structure but produces nothing.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #0 : IF 1 : LDA #2 : ENDIF : LDA #3"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x02, 0xA9, 0x03}, 6));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #0 : IF 0 : LDA #2 : ENDIF : LDA #3"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x03}, 4));
}

RC_TEST_STEP(assemble, if_elif_else_chain, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF 1 : LDA #1 : ELIF 1 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x01}, 2));   // first true wins
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF 0 : LDA #1 : ELIF 1 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x02}, 2));   // the elif
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF 0 : LDA #1 : ELIF 0 : LDA #2 : ELSE : LDA #3 : ENDIF"),
                          (uint8_t[]){0xA9, 0x03}, 2));   // the else
}

RC_TEST_STEP(assemble, if_does_not_introduce_scope, fix)
{
    // A label set in a live IF branch leaks to the enclosing scope (IF is not a brace). The section org keeps
    // `here` out of the zero page so `LDA here` is absolute (3 bytes).
    uint32_t passes = ASM("SECTION s, org=&2000 : IF 1 : .here : ENDIF : LDA here : ENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(code_in_is(&fix->r, passes, 1, (uint8_t[]){0xAD, 0x00, 0x20}, 3));
}

RC_TEST_STEP(assemble, if_wraps_scope_and_nests, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF 1 : { LDA #5 } : ENDIF"), (uint8_t[]){0xA9, 0x05}, 2));
    RC_CHECK(((void)ASM("IF 0 : { LDA #5 } : ENDIF"), baron_result_code(&fix->r).num), ==, 0u);
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF 1 : IF 1 : LDA #7 : ENDIF : ENDIF"), (uint8_t[]){0xA9, 0x07}, 2));
    RC_CHECK(((void)ASM("IF 1 : IF 0 : LDA #7 : ENDIF : ENDIF"), baron_result_code(&fix->r).num), ==, 0u);
    RC_CHECK(((void)ASM("IF 0 : IF 1 : LDA #7 : ENDIF : ENDIF"), baron_result_code(&fix->r).num), ==, 0u);   // outer dead -> inner dead
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
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, if_branch_flip_clears_label, fix)
{
    // `gate` is 1 on the pass after `.other` first appears, then 0 once defined(other) sees it - so
    // `.x` is bound on one pass and must be CLEARED when its branch goes inactive. The remove-on-
    // inactive rule leaves it undefined; a stale binding would linger.
    RC_CHECK_TRUE(ASM("IF gate : .x : ENDIF\ngate = 1 - defined(other)\n.other") != 0);
    RC_CHECK_TRUE(value_is_none(baron_result_symbol(&fix->r, RC_STR("x"))));
}

RC_TEST_STEP(assemble, dead_branch_does_not_clobber_live_binding, fix)
{
    // The inactive ELSE assigns `blah` too; it must NOT delete the live IF branch's binding (each dead-branch
    // assignment clears only what it owns). Likewise a dead branch must leave an OUTER binding of the name intact.
    RC_CHECK_TRUE(ASM("IF 1 : blah = 2 : ELSE : blah = 3 : ENDIF") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("blah")), value_make_numeric(2)));

    RC_CHECK_TRUE(ASM("x = 1\nIF 0 : x = 2 : ENDIF") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("x")), value_make_numeric(1)));
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
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA9, 0x01, 0xEA, 0xA5, 0x05, 0x60}, 6));
}

RC_TEST_STEP(assemble, if_forward_ref_both_fixed_points_valid, fix)
{
    // Both "taken" (fwdlabel=10) and "skipped" (fwdlabel=5) are self-consistent fixed points. We
    // converge to whichever our first guess lands on: an unresolved operand resolves to zero-page
    // optimistically (smallest), so fwdlabel starts at 5, `5 > 5` is false, and we settle on skipped.
    uint32_t passes = ASM("LDA #1 : IF fwdlabel > 5 : LDA #2 : JSR &FFEE : ENDIF : NOP : LDA fwdlabel : .fwdlabel : RTS");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA9, 0x01, 0xEA, 0xA5, 0x05, 0x60}, 6));
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
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA9, 0x00, 0xEA, 0xEA, 0xA2, 0x01, 0xA0, 0x02, 0x60}, 9));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("end")), value_make_numeric(8)));
}

RC_TEST_STEP(assemble, org_from_forward_ref_ends_at_address, fix)
{
    // The "make this block end at address X" idiom: a section `org` computed from a label defined AFTER it, so
    // the 8-byte block sits at &0FF8..&0FFF and progend lands exactly on &1000. The org attribute is unresolved
    // on the first pass (progstart/progend unknown) and settles once they do; JMP progstart then carries the
    // relocated address. (Code itself still fills the section buffer from index 0.)
    uint32_t passes = ASM("SECTION s, org = &1000 - (progend - progstart)\n"
                          ".progstart LDA #&41 : JSR &FFEE : JMP progstart : .progend\n"
                          "ENDSECTION");
    RC_CHECK_TRUE(code_in_is(&fix->r, passes, 1, (uint8_t[]){0xA9, 0x41, 0x20, 0xEE, 0xFF, 0x4C, 0xF8, 0x0F}, 8));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("progstart")), value_make_numeric(0x0FF8)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("progend")), value_make_numeric(0x1000)));
}

RC_TEST_STEP(assemble, for_repeats_body, fix)
{
    // FOR runs its body once per element, emitting a fresh copy each time.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FOR i = 0..2 : NOP : NEXT"), (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
    // An exclusive range is the same minus its endpoint.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FOR i = 0..<3 : NOP : NEXT"), (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
}

RC_TEST_STEP(assemble, for_binds_loop_variable, fix)
{
    // The loop variable holds the current element, visible to the body. 0..3 is inclusive (4 elements).
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FOR i = 0..3 : LDA #i : NEXT"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01, 0xA9, 0x02, 0xA9, 0x03}, 8));
    // A list literal drives the loop just as a range does.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FOR x = {10, 20, 30} : LDA #x : NEXT"),
                          (uint8_t[]){0xA9, 0x0A, 0xA9, 0x14, 0xA9, 0x1E}, 6));
}

RC_TEST_STEP(assemble, for_iteration_has_its_own_scope, fix)
{
    // A label in the body is redefined every iteration; each iteration's private scope keeps that from
    // being a duplicate.
    uint32_t passes = ASM("FOR i = 0..2 : .lbl NOP : NEXT");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
}

RC_TEST_STEP(assemble, for_nests, fix)
{
    // Two iterations of two iterations: four bodies.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FOR i = 0..1 : FOR j = 0..1 : NOP : NEXT : NEXT"),
                          (uint8_t[]){0xEA, 0xEA, 0xEA, 0xEA}, 4));
}

RC_TEST_STEP(assemble, for_empty_sequence_runs_zero_times, fix)
{
    // An empty sequence is legal and assembles the body zero times (no error): the surrounding code is
    // emitted as if the FOR were absent. Both the empty list literal and a non-ascending exclusive range.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #0 : FOR i = {} : NOP : NEXT : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #0 : FOR i = 5..<5 : NOP : NEXT : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
}

RC_TEST_STEP(assemble, for_forward_ref_count_defers, fix)
{
    // When the count depends on a forward reference, the FOR runs zero times on the first pass and
    // re-expands once the bound resolves.
    uint32_t passes = ASM("FOR i = 0..n : NOP : NEXT\nn = 2");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xEA, 0xEA, 0xEA}, 3));
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
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB 1, 2, 3"), (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB &FF"),     (uint8_t[]){0xFF}, 1));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB 255"),     (uint8_t[]){0xFF}, 1));
}

RC_TEST_STEP(assemble, equb_allows_signed_bytes, fix)
{
    // Negative numbers are allowed in -255..255; we emit the low byte.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB -1"),   (uint8_t[]){0xFF}, 1));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB -255"), (uint8_t[]){0x01}, 1));
    RC_CHECK_TRUE(ERR("EQUB 256")  == error_type_value_out_of_range);
    RC_CHECK_TRUE(ERR("EQUB -256") == error_type_value_out_of_range);
}

RC_TEST_STEP(assemble, equs_and_equb_are_aliases, fix)
{
    // EQUS spells out a string char-by-char...
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUS \"ABC\""), (uint8_t[]){0x41, 0x42, 0x43}, 3));
    // ...but takes numbers too, and EQUB takes strings - they are the same directive.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUS 65, 66"),  (uint8_t[]){0x41, 0x42}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB \"Hi\", 0"), (uint8_t[]){0x48, 0x69, 0x00}, 3));
}

RC_TEST_STEP(assemble, equb_ranges_and_lists, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB 1..3"),  (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB 0..<3"), (uint8_t[]){0x00, 0x01, 0x02}, 3));
    // A list is flattened (nested lists and ranges descend); a string element stays whole.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB {1, 2, 3}"),       (uint8_t[]){0x01, 0x02, 0x03}, 3));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB {1, {2, 3}, 4}"),  (uint8_t[]){0x01, 0x02, 0x03, 0x04}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB {1, \"Hi\", 2}"),  (uint8_t[]){0x01, 0x48, 0x69, 0x02}, 4));
}

RC_TEST_STEP(assemble, equb_advances_pc, fix)
{
    // The three bytes move pc, so a label after them sees the advanced address (default section, org 0).
    RC_CHECK_TRUE(ASM("EQUB 1, 2, 3 : .here") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(3)));
}

RC_TEST_STEP(assemble, equb_forward_reference, fix)
{
    // A forward reference is one byte (a placeholder until it resolves): end - start = 1.
    uint32_t passes = ASM(".start EQUB end - start : .end");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0x01}, 1));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, equb_dead_branch_emits_nothing, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #0 : IF 0 : EQUB 1, 2, 3 : ENDIF : LDA #1"),
                          (uint8_t[]){0xA9, 0x00, 0xA9, 0x01}, 4));
}

RC_TEST_STEP(assemble, equw_emits_little_endian_words, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW &1234"),   (uint8_t[]){0x34, 0x12}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW 1, 2"),    (uint8_t[]){0x01, 0x00, 0x02, 0x00}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW 258"),     (uint8_t[]){0x02, 0x01}, 2));
    // Ranges and lists flatten, each element a 16-bit word.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW 0..2"),        (uint8_t[]){0x00, 0x00, 0x01, 0x00, 0x02, 0x00}, 6));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW {1, {2, 3}}"), (uint8_t[]){0x01, 0x00, 0x02, 0x00, 0x03, 0x00}, 6));
}

RC_TEST_STEP(assemble, equw_signed_range_and_pc, fix)
{
    // Signed window is -65535..65535; we emit the low word.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW -1"),     (uint8_t[]){0xFF, 0xFF}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUW 65535"),  (uint8_t[]){0xFF, 0xFF}, 2));
    RC_CHECK_TRUE(ERR("EQUW 65536")  == error_type_value_out_of_range);
    RC_CHECK_TRUE(ERR("EQUW -65536") == error_type_value_out_of_range);
    // Each word advances pc by 2; a label after two words sees +4 (default section, org 0).
    RC_CHECK_TRUE(ASM("EQUW 1, 2 : .here") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(4)));
    // A forward reference is a 2-byte placeholder, so end - start = 2.
    uint32_t passes = ASM(".start EQUW end - start : .end");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0x02, 0x00}, 2));
    RC_CHECK_TRUE(passes >= 2u);
}

RC_TEST_STEP(assemble, equd_emits_little_endian_dwords, fix)
{
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUD &12345678"), (uint8_t[]){0x78, 0x56, 0x34, 0x12}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUD 1, 2"),
                          (uint8_t[]){0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}, 8));
    // Signed window is -(2^32-1)..(2^32-1); we emit the low dword.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUD -1"), (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUD 4294967295"), (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF}, 4));
    RC_CHECK_TRUE(ERR("EQUD 4294967296") == error_type_value_out_of_range);
    // Each dword advances pc by 4 (default section, org 0).
    RC_CHECK_TRUE(ASM("EQUD 1 : .here") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(4)));
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
    RC_CHECK(fix->r.diagnostics.num, ==, 3u);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->r.diagnostics, 0).code == error_type_value_out_of_range);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->r.diagnostics, 1).code == error_type_value_out_of_range);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->r.diagnostics, 2).code == error_type_bad_addressing_mode);
    // A syntax (fatal) error, by contrast, aborts the whole assemble with just itself.
    RC_CHECK(((void)ASM("} ENDIF NEXT"), fix->r.diagnostics.num), ==, 1u);
}

RC_TEST_STEP(assemble, duplicate_symbol_signposts_original, fix)
{
    // A duplicate raises the error at the redefinition AND a companion note pointing back at the first
    // binding (both severity-0 errors, so both always show). The note's location precedes the error's.
    RC_CHECK_TRUE(ASM(".dup NOP : .dup RTS") == 0);
    RC_CHECK(fix->r.diagnostics.num, ==, 2u);
    diagnostic err  = rc_view_diagnostic_get(fix->r.diagnostics, 0);
    diagnostic note = rc_view_diagnostic_get(fix->r.diagnostics, 1);
    RC_CHECK_TRUE(err.code == error_type_duplicate_symbol);
    RC_CHECK_TRUE(note.code == error_type_original_definition);
    RC_CHECK_TRUE(note.at.pos < err.at.pos);   // the original sits earlier in the source than the redefinition
    // Same for assignments.
    RC_CHECK_TRUE(ERR("twice = 1 : twice = 2") == error_type_duplicate_symbol);
    RC_CHECK(fix->r.diagnostics.num, ==, 2u);
    RC_CHECK_TRUE(rc_view_diagnostic_get(fix->r.diagnostics, 1).code == error_type_original_definition);
}

RC_TEST_STEP(assemble, failure_clears_outputs, fix)
{
    // A failed assemble hands back no object code and no symbols - only the diagnostics remain.
    RC_CHECK_TRUE(ASM(".lbl LDA #0 : TAX #5") == 0);   // TAX #5 has no encoding
    RC_CHECK(baron_result_code(&fix->r).num, ==, 0u);
    RC_CHECK_TRUE(value_is_none(baron_result_symbol(&fix->r, RC_STR("lbl"))));
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_bad_addressing_mode));
}

RC_TEST_STEP(assemble, warnings_do_not_fail_assembly, fix)
{
    // JMP (&xxFF) is legal but trips the NMOS vector-fetch page-wrap bug: a warning, not an error, so the
    // assemble still succeeds and emits the three bytes - the warning just rides along in the diagnostics.
    uint32_t passes = ASM("JMP (&12FF)");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0x6C, 0xFF, 0x12}, 3));
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);   // no error-severity diagnostic
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_jmp_indirect_page_cross));
    // A vector that does not straddle a page boundary is silent.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("JMP (&1234)"), (uint8_t[]){0x6C, 0x34, 0x12}, 3));
    RC_CHECK(fix->r.diagnostics.num, ==, 0u);
}

RC_TEST_STEP(assemble, named_constants, fix)
{
    // A pure constant flows through the operand path like any number...
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #TRUE"),  (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #FALSE"), (uint8_t[]){0xA9, 0x00}, 2));
    // ...and serves as an IF condition.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF TRUE : LDA #1 : ELSE : LDA #2 : ENDIF"),  (uint8_t[]){0xA9, 0x01}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF FALSE : LDA #1 : ELSE : LDA #2 : ENDIF"), (uint8_t[]){0xA9, 0x02}, 2));
    // The constants are reserved: you cannot redefine one (it would otherwise bind a symbol shadowed by the
    // constant in every expression).
    RC_CHECK_TRUE(ERR("PI = 5") == error_type_reserved_constant);
    RC_CHECK_TRUE(ERR("TRUE = 1") == error_type_reserved_constant);
    RC_CHECK_TRUE(ERR("FALSE = 0") == error_type_reserved_constant);
}

RC_TEST_STEP(assemble, pc_constant, fix)
{
    // '*' (and its BBC Micro alias P%) is the current PC. It is evaluated before the instruction emits, so
    // JMP * is the classic jump-to-self. (A section org puts * at &2000; the code lands in section index 1.)
    RC_CHECK_TRUE(code_in_is(&fix->r, ASM("SECTION s, org=&2000 : JMP * : ENDSECTION"),  1, (uint8_t[]){0x4C, 0x00, 0x20}, 3));
    RC_CHECK_TRUE(code_in_is(&fix->r, ASM("SECTION s, org=&2000 : JMP P% : ENDSECTION"), 1, (uint8_t[]){0x4C, 0x00, 0x20}, 3));
    // Mid-program it reads the live PC: after LDA #0 (two bytes), * is org+2.
    uint32_t passes = ASM("SECTION s, org=&2000 : LDA #0 : here = * : RTS : ENDSECTION");
    RC_CHECK_TRUE(code_in_is(&fix->r, passes, 1, (uint8_t[]){0xA9, 0x00, 0x60}, 3));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(0x2002)));
    // A comma list evaluates one element at a time, so the PC advances between them (org 0 here: 0, 1, 2)...
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB *, *, *"), (uint8_t[]){0x00, 0x01, 0x02}, 3));
    // ...but a list literal is one value computed at one instant, so every * is the same PC.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB {*, *, *}"), (uint8_t[]){0x00, 0x00, 0x00}, 3));
}

RC_TEST_STEP(assemble, random, fix)
{
    // full fills a run on its own; RND(1) is always 0, so the broadcast idiom RND(full(n,1)) is n zeroes.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB full(4, &FF)"),      (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF}, 4));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB RND(1), RND(1)"),    (uint8_t[]){0x00, 0x00}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB RND(full(4, 1))"),   (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4));

    // The definitive reset test: the same source assembles to the same bytes twice. Without the per-pass
    // reseed the second run's stream would continue from where the first left off and diverge.
    uint32_t p1 = ASM("EQUB RND(full(3, 200))");
    rc_view_bytes first = baron_result_code(&fix->r);
    RC_CHECK_TRUE(p1 != 0 && first.num == 3);
    uint8_t saved[3];
    for (uint32_t i = 0; i < 3; i++) {
        saved[i] = rc_view_bytes_get(first, i);
    }
    uint32_t p2 = ASM("EQUB RND(full(3, 200))");
    rc_view_bytes second = baron_result_code(&fix->r);
    RC_CHECK_TRUE(p2 != 0 && second.num == 3);
    for (uint32_t i = 0; i < 3; i++) {
        RC_CHECK(rc_view_bytes_get(second, i), ==, saved[i]);
    }

    // A larger draw list converges and stays in-range; RND(0) has no range and is a domain error.
    uint32_t p8 = ASM("EQUB RND(full(8, 256))");
    RC_CHECK_TRUE(p8 != 0 && baron_result_code(&fix->r).num == 8);
    RC_CHECK_TRUE(((void)ASM("EQUB RND(0)"), has_diag(&fix->r, error_type_domain)));
}

// INCLUDE tests give the top source an explicit slash-free name: ASM names a source after its own text,
// which would drop the include's own slashes into the cache key and wreck the relative-path peel. Like ASM,
// this stashes the result in fix->r and yields the pass count.
#define INC(src) (fix->r = assemble_string(&fix->arenas, RC_STR("top"), RC_STR(src)), fix->r.passes)

RC_TEST_STEP(assemble, include_splices_file, fix)
{
    // INCLUDE pulls the file in textually: its instruction emits right here, and its label binds in THIS
    // scope (no scope of its own).
    uint32_t passes = INC("include \"inc_child.6502\"");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA2, 0x02}, 2));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("child")), value_make_numeric(0)));
}

RC_TEST_STEP(assemble, include_resolves_relative_to_includer, fix)
{
    // sub/mid.6502 does its own INCLUDE "leaf.6502" - resolved against sub/, not the top file's directory.
    uint32_t passes = INC("include \"sub/mid.6502\"");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA0, 0x03, 0xEA}, 3));
}

RC_TEST_STEP(assemble, include_joins_the_multi_pass, fix)
{
    // `target` sits after the include, so its address depends on the included file's two bytes - it only
    // settles once the include is part of the pass loop. JMP is fixed-width, so this converges cleanly.
    uint32_t passes = INC("jmp target : include \"inc_fwd_child.6502\" : .target rts");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0x4C, 0x05, 0x00, 0xEA, 0xEA, 0x60}, 6));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("target")), value_make_numeric(5)));
}

RC_TEST_STEP(assemble, include_reports_included_from_frame, fix)
{
    // The included file has an out-of-range immediate. That error surfaces, followed by a frame pointing
    // back at the INCLUDE - so a failure inside an include names both the real site and how we reached it.
    RC_CHECK(INC("include \"inc_bad_child.6502\""), ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_value_out_of_range));
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_included_from));
}

RC_TEST_STEP(assemble, include_missing_file_is_an_error, fix)
{
    RC_CHECK(INC("include \"no_such_baron_file.6502\""), ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_source_load));
}

RC_TEST_STEP(assemble, include_forward_declared_filename, fix)
{
    // The filename is a symbol only bound AFTER the INCLUDE: it defers on pass 1 (like a forward address)
    // and the file loads once the name settles.
    uint32_t passes = INC("include fname : fname = \"inc_child.6502\"");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA2, 0x02}, 2));
    // A name that never binds (nothing defines it) is an undefined symbol at the INCLUDE, not a hang.
    RC_CHECK((fix->r = assemble_string(&fix->arenas, RC_STR("top2"), RC_STR("include missing_name"))).passes, ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_undefined_symbol));
}

RC_TEST_STEP(assemble, include_in_dead_branch_is_skipped, fix)
{
    // A dead INCLUDE never even goes looking for its file, so a missing include under IF 0 assembles clean.
    uint32_t passes = INC("if 0 : include \"no_such_baron_file.6502\" : endif");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK(fix->r.diagnostics.num, ==, 0u);
}

RC_TEST_STEP(assemble, include_cycle_is_caught, fix)
{
    // inc_cycle.6502 includes itself; the depth cap stops the recursion rather than blowing the C stack.
    uint32_t passes = (fix->r = assemble_file(&fix->arenas, RC_STR("inc_cycle.6502"))).passes;
    RC_CHECK(passes, ==, 0u);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_include_too_deep));
}

RC_TEST_STEP(assemble, incbin_splices_file_bytes, fix)
{
    // Write a small binary, splice it, and check the exact bytes land in the section and pc advances by its
    // size (a label after INCBIN sees +size). One assemble - INC dedupes sources by name, so we cannot reuse it.
    uint8_t blob[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42};
    RC_CHECK_TRUE(rc_file_save_binary(RC_STR("blob.bin"),
                  (rc_view_bytes) {.data = blob, .num = (uint32_t) sizeof blob}) == RC_FILE_OK);

    uint32_t passes = (fix->r = assemble_string(&fix->arenas, RC_STR("t"),
                          RC_STR("incbin \"blob.bin\" : .after"))).passes;   // default section, org 0
    RC_CHECK_TRUE(code_is(&fix->r, passes, blob, (uint32_t) sizeof blob));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("after")),
                                 value_make_numeric(sizeof blob)));

    rc_file_delete(RC_STR("blob.bin"));
}

RC_TEST_STEP(assemble, incbin_missing_file_is_fatal, fix)
{
    // A missing file cannot be recovered from (the output would be the wrong size), so it is fatal.
    RC_CHECK(INC("incbin \"no_such_blob.bin\""), ==, 0u);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_source_load);
}

RC_TEST_STEP(assemble, macro_expands_body, fix)
{
    // A directive body with a value parameter.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("MACRO PAD n : SKIP n : ENDMACRO\nPAD 3"),
                          (uint8_t[]) {0x00, 0x00, 0x00}, 3));
    // Instructions with the parameter woven through the operands.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("MACRO ST16 a : LDA #0 : STA a : STA a+1 : ENDMACRO\nST16 &70"),
                          (uint8_t[]) {0xA9, 0x00, 0x85, 0x70, 0x85, 0x71}, 6));
}

RC_TEST_STEP(assemble, macro_overload_by_arity, fix)
{
    // TWO a, b and TWO a are distinct overloads; the call picks by the number of arguments.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("MACRO TWO a, b : EQUB a : EQUB b : ENDMACRO\n"
            "MACRO TWO a : EQUB a : EQUB a : ENDMACRO\n"
            "TWO 5, 6\nTWO 9"),
        (uint8_t[]) {0x05, 0x06, 0x09, 0x09}, 4));
}

RC_TEST_STEP(assemble, macro_overload_by_literal_token, fix)
{
    // The immediate form carries a "#" literal; ADD 1, #2 picks it, ADD 1, 2 the two-argument form.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("MACRO ADD a, \"#\" b : EQUB a : EQUB b : ENDMACRO\n"
            "MACRO ADD a, b : EQUB a : EQUB &FF : ENDMACRO\n"
            "ADD 1, #2\nADD 1, 2"),
        (uint8_t[]) {0x01, 0x02, 0x01, 0xFF}, 4));
}

RC_TEST_STEP(assemble, macro_matches_tokens_before_expressions, fix)
{
    // LAX "(" addr ")" outranks LAX addr, so a parenthesised call takes the token form even though the
    // bare form's greedy expression would also swallow (5).
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("MACRO LAX \"(\" addr \")\" : EQUB addr : EQUB &EE : ENDMACRO\n"
            "MACRO LAX addr : EQUB addr : ENDMACRO\n"
            "LAX (5)\nLAX 7"),
        (uint8_t[]) {0x05, 0xEE, 0x07}, 3));
}

RC_TEST_STEP(assemble, macro_forward_referenced_argument, fix)
{
    // The argument is a symbol defined AFTER the call: it defers, then resolves on a later pass.
    uint32_t passes = ASM("MACRO W a : EQUB a : ENDMACRO\nW later\nlater = 5");
    RC_CHECK_TRUE(passes >= 2);
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0x05}, 1));
}

RC_TEST_STEP(assemble, macro_body_labels_are_per_invocation, fix)
{
    // A body that defines a label, invoked twice: each expansion has its own scope, so no duplicate.
    uint32_t passes = ASM("MACRO M : .here : NOP : ENDMACRO\nM\nM");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0xEA, 0xEA}, 2));
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_duplicate_symbol));
}

RC_TEST_STEP(assemble, macro_bounded_self_recursion, fix)
{
    // FILL n recurses with an IF base case; the recursive call goes inactive at n == 0 and stops.
    uint32_t passes = ASM("MACRO FILL n : IF n > 0 : EQUB n : FILL n-1 : ENDIF : ENDMACRO\nFILL 3");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0x03, 0x02, 0x01}, 3));
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_macro_too_deep));
}

RC_TEST_STEP(assemble, macro_mutual_recursion_via_forward_declaration, fix)
{
    // PING is forward-declared (empty body) so PONG can call it; the real PING fills that in, not a
    // duplicate. PING 3 then ping-pongs down to zero.
    uint32_t passes = ASM("MACRO PING n : ENDMACRO\n"
                          "MACRO PONG n : IF n > 0 : EQUB n : PING n-1 : ENDIF : ENDMACRO\n"
                          "MACRO PING n : IF n > 0 : EQUB n : PONG n-1 : ENDIF : ENDMACRO\n"
                          "PING 3");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0x03, 0x02, 0x01}, 3));
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_duplicate_signature));
}

RC_TEST_STEP(assemble, macro_error_breadcrumb, fix)
{
    // A body whose EQUB overflows raises value_out_of_range at the definition, then an expanded_from
    // frame at the call site.
    ASM("MACRO BIG x : EQUB x : ENDMACRO\nBIG 300");
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_value_out_of_range));
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_expanded_from));
}

RC_TEST_STEP(assemble, macro_framing_and_call_errors, fix)
{
    // A call before the definition: the name is not a token yet, so it misparses as an assignment.
    RC_CHECK(ASM("PAD 3\nMACRO PAD n : SKIP n : ENDMACRO"), ==, 0u);
    // No overload fits the arguments.
    RC_CHECK_TRUE(ERR("MACRO AD a, b : EQUB a : ENDMACRO\nAD 1, 2, 3") == error_type_no_matching_signature);
    // Unbounded recursion (no base case) trips the depth cap.
    RC_CHECK_TRUE(ERR("MACRO LOOP n : EQUB n : LOOP n : ENDMACRO\nLOOP 1") == error_type_macro_too_deep);
    // A macro invoked while only forward-declared has no body to expand here.
    RC_CHECK_TRUE(ERR("MACRO F n : ENDMACRO\nF 5") == error_type_macro_not_defined);
    // Missing ENDMACRO.
    RC_CHECK_TRUE(ERR("MACRO G n : EQUB n") == error_type_unclosed_macro);
    // Two real bodies for one signature.
    RC_CHECK_TRUE(ERR("MACRO H n : EQUB 1 : ENDMACRO\nMACRO H n : EQUB 2 : ENDMACRO") == error_type_duplicate_signature);
    // A header with no separator before ENDMACRO.
    RC_CHECK_TRUE(ERR("MACRO A n ENDMACRO") == error_type_expected_separator);
    // A macro cannot be named after a mnemonic.
    RC_CHECK_TRUE(ERR("MACRO NOP : ENDMACRO") == error_type_macro_name_reserved);
}

RC_TEST_STEP(assemble, function_single_line, fix)
{
    // A one-line value function, called in an operand position.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FUNCTION sqr(x) = x*x\nEQUB sqr(5)"), (uint8_t[]) {25}, 1));
    // The parameter woven through a bigger expression (`and` is baron's bitwise AND; `lo` is a builtin, so `low`).
    RC_CHECK_TRUE(code_is(&fix->r, ASM("FUNCTION low(w) = w and &FF\nEQUB low(&1234)"), (uint8_t[]) {0x34}, 1));
    // Lexical scope: a body sees a global defined where the function was written.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("k = 10\nFUNCTION addk(x) = x + k\nEQUB addk(5)"), (uint8_t[]) {15}, 1));
}

RC_TEST_STEP(assemble, function_multiline_if, fix)
{
    // A body with an IF choosing the top-level return, plus a function-local assignment.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("FUNCTION clamp(x) : IF x > 9 : r = 9 : ELSE : r = x : ENDIF : = r\nEQUB clamp(20)\nEQUB clamp(3)"),
        (uint8_t[]) {9, 3}, 2));
}

RC_TEST_STEP(assemble, function_recursive_gcd, fix)
{
    // Euclid's algorithm, recursive, with a function-local and a top-level return.
    uint32_t passes = ASM("FUNCTION gcd(a, b) : IF b = 0 : r = a : ELSE : r = gcd(b, a mod b) : ENDIF : = r\n"
                          "EQUB gcd(48, 36)\nEQUB gcd(1071, 462)");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {12, 21}, 2));
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);   // no runaway-recursion trip
}

RC_TEST_STEP(assemble, function_overload_by_arity, fix)
{
    // area(w) and area(w,h): the call picks the overload by argument count.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("FUNCTION area(w) = w*w\nFUNCTION area(w, h) = w*h\nEQUB area(3)\nEQUB area(3, 4)"),
        (uint8_t[]) {9, 12}, 2));
}

RC_TEST_STEP(assemble, function_mutual_recursion_via_forward_declaration, fix)
{
    // Parity by mutual recursion - a genuinely useful pair - needs a forward declaration so is_even can name
    // is_odd before is_odd is defined. The fill-in of the same arity is not a duplicate.
    uint32_t passes = ASM("FUNCTION is_odd(n) =\n"
                          "FUNCTION is_even(n) : IF n = 0 : r = 1 : ELSE : r = is_odd(n - 1)  : ENDIF : = r\n"
                          "FUNCTION is_odd(n)  : IF n = 0 : r = 0 : ELSE : r = is_even(n - 1) : ENDIF : = r\n"
                          "EQUB is_even(10)\nEQUB is_odd(7)\nEQUB is_even(3)");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {1, 1, 0}, 3));
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, function_recursive_list, fix)
{
    // A recursive fold over a list to a scalar.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("FUNCTION rsum(xs) : IF len(xs) = 0 : r = 0 : ELSE : r = xs[0] + rsum(xs[1..]) : ENDIF : = r\n"
            "EQUB rsum({1, 2, 3, 4})"),
        (uint8_t[]) {10}, 1));

    // A recursive build of a NEW list (reverse), emitted as bytes.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("FUNCTION rrev(xs) : IF len(xs) = 0 : r = xs : ELSE : r = concat(rrev(xs[1..]), {xs[0]}) : ENDIF : = r\n"
            "EQUB rrev({1, 2, 3})"),
        (uint8_t[]) {3, 2, 1}, 3));
}

RC_TEST_STEP(assemble, function_recursive_quicksort, fix)
{
    // Quicksort, in functions: two recursive partition helpers and a recursive sort that sorts
    // both partitions, using variadic concat, unbounded-range tails xs[1..], and empty / singleton list literals.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("FUNCTION lt(xs, p)                          \n\
                 IF len(xs) = 0                          \n\
                     r = {}                              \n\
                 ELIF xs[0] < p                          \n\
                     r = concat({xs[0]}, lt(xs[1..], p)) \n\
                 ELSE                                    \n\
                     r = lt(xs[1..], p)                  \n\
                 ENDIF                                   \n\
             = r                                         \n\
                                                         \n\
             FUNCTION ge(xs, p)                          \n\
                 IF len(xs) = 0                          \n\
                     r = {}                              \n\
                 ELIF xs[0] < p                          \n\
                     r = ge(xs[1..], p)                  \n\
                 ELSE                                    \n\
                     r = concat({xs[0]}, ge(xs[1..], p)) \n\
                 ENDIF                                   \n\
             = r                                         \n\
                                                         \n\
             FUNCTION qsort(xs)                          \n\
                 IF len(xs) <= 1                         \n\
                     r = xs                              \n\
                 ELSE                                    \n\
                     r = concat(qsort(lt(xs[1..], xs[0])), {xs[0]}, qsort(ge(xs[1..], xs[0]))) \n\
                 ENDIF                                   \n\
             = r                                         \n\
                                                         \n\
            EQUB qsort({3, 1, 4, 1, 5, 9, 2, 6})"
        ),
        (uint8_t[]) {1, 1, 2, 3, 4, 5, 6, 9}, 8));
}

RC_TEST_STEP(assemble, function_forward_referenced_argument, fix)
{
    // An argument that is a forward reference resolves over passes, like any operand.
    uint32_t passes = ASM("FUNCTION sqr(x) = x*x\nEQUB sqr(later)\nlater = 5");
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {25}, 1));
    RC_CHECK(passes, >=, 2u);
}

RC_TEST_STEP(assemble, function_scope_is_per_invocation, fix)
{
    // A body local, and two calls: each invocation gets its own child scope, so the local does not collide.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("FUNCTION dbl(x) : y = x + x : = y\nEQUB dbl(3)\nEQUB dbl(4)"),
        (uint8_t[]) {6, 8}, 2));
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);   // no duplicate_symbol across the two calls
}

RC_TEST_STEP(assemble, function_scoping_is_lexical, fix)
{
    // A body sees the scope where it was DEFINED (a global at root), not the caller's locals. `usesg` reads
    // the global g = 7; `caller` binds its OWN param g = 99 and calls usesg. Lexical scoping means usesg
    // resolves g to the root global (7), NOT the caller's g (99) - so caller(99) is 0 + 7 = 7. Dynamic
    // scoping would leak the caller's 99. This is the load-bearing check that scoping is lexical.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("g = 7\nFUNCTION usesg(x) = x + g\nFUNCTION caller(g) = usesg(0)\nEQUB caller(99)"),
        (uint8_t[]) {7}, 1));
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, function_call_and_definition_errors, fix)
{
    // A call before the definition: the name is not an operand token yet, so it misparses.
    RC_CHECK(ASM("EQUB sqr(5)\nFUNCTION sqr(x) = x*x"), ==, 0u);
    // No overload takes this argument count.
    RC_CHECK_TRUE(ERR("FUNCTION area(w) = w*w\nEQUB area(1, 2)") == error_type_no_matching_arity);
    // A forward declaration that is invoked before it is filled in.
    RC_CHECK_TRUE(ERR("FUNCTION f(n) =\nEQUB f(3)") == error_type_function_not_defined);
    // Two real bodies for one arity.
    RC_CHECK_TRUE(ERR("FUNCTION g(x) = 1\nFUNCTION g(x) = 2") == error_type_duplicate_function);
    // Unbounded recursion (no base case) trips the depth cap.
    RC_CHECK_TRUE(ERR("FUNCTION loop(n) = loop(n) + 1\nEQUB loop(1)") == error_type_function_too_deep);
    // A function cannot be named after a mnemonic, nor after a builtin operand function.
    RC_CHECK_TRUE(ERR("FUNCTION nop(x) = 1") == error_type_function_name_reserved);
    RC_CHECK_TRUE(ERR("FUNCTION lo(x) = x") == error_type_function_name_reserved);
}

RC_TEST_STEP(assemble, stress_many_symbols_and_scopes, fix)
{
    // Push the permanent arena well past its initial reserves so its scope nodes and symbol pool have to
    // grow and relocate mid-assembly, then confirm nothing dangles: every symbol is still addressable by
    // name once the containers have moved. This is the safety net for the whole indices-not-pointers
    // invariant behind the arena merge - a big FOR mints one child scope per iteration (unspellable names),
    // and hundreds of top-level labels swell the global symbol trie.
    rc_arena build = rc_arena_make_default();
    rc_mstr  src   = rc_mstr_make(64 * 1024, &build);

    // The default section starts at org 0. Each iteration binds its own `n` in a per-iteration scope and
    // emits a byte, so pc keeps advancing.
    rc_mstr_append(&src, RC_STR("FOR n = 0..1999\nEQUB 42\nNEXT\n"), &build);
    // Hundreds of distinct global labels, all sitting at pc 2000 (nothing emits between them).
    for (uint32_t i = 0; i < 500; i++) {
        rc_mstr_append(&src, RC_STR(".lbl"), &build);
        rc_mstr_append_u32(&src, i, &build);
        rc_mstr_append_char(&src, '\n', &build);
    }
    rc_mstr_append(&src, RC_STR("last = &BEEF\n"), &build);

    uint32_t passes = (fix->r = assemble_string(&fix->arenas, RC_STR("stress"), src.view)).passes;
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK(baron_result_code(&fix->r).num, ==, 2000u);   // 2000 bytes from the loop; the labels emit nothing
    // A symbol bound last, after every relocation, is still correct...
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("last")), value_make_numeric(0xBEEF)));
    // ...as is a label from the middle of the run (they all resolve to the post-loop pc).
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("lbl250")), value_make_numeric(2000)));

    rc_arena_deinit(&build);
}

#endif // BARON_TESTS
