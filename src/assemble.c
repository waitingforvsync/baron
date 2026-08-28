#include "assemble_internal.h"   // the parsing vocabulary (and assemble.h via it)

#include "opcodes.h"
#include "lexer.h"
#include "basic.h"               // the BBC BASIC 4 line tokeniser behind BASIC ... ENDBASIC
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
static parse_result handle_skip(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_skipto(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_align(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_section(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_zpreserve(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_zpauto1(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_zpauto2(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_zpauto_n(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_unreachable(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_cancall(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_canjump(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_discard(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_zpentry(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_zpinterrupt(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_equb(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_equw(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_equd(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_label(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_local_label(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_open_brace(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_if(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_for(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_include(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_incbin(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_incsection(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_basic(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_macro(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_macro_invocation(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, uint32_t macro_index, rc_arena scratch);
static parse_result handle_function(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_reserved_constant(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_print(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result handle_error(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result parse_block(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result parse_file(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result parse_scope(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);
static parse_result parse_one_statement(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);


// int_argument_make is shared with opcodes.c (both declared in assemble.h); it needs no token
// table, so it sits on its own. Pure: it reads v and returns the reduction.
int_argument int_argument_make(value v, bool final_pass, uint32_t at)
{
    if (value_is_number(v)) {   // a boolean coerces: TRUE / FALSE are 1 / 0 here
        return (int_argument) {
            .type = int_argument_type_known,
            .value = (int64_t) v.numeric
        };
    }

    if (value_is_zpauto(v)) {
        // A ZPAUTO address: known as far as sizing goes (the offset stands in, and the real base
        // cannot leave the zero page), with the identity riding along for the caller to accept -
        // an instruction operand, a data byte - or refuse - a count, a condition, a layout address.
        return (int_argument) {
            .type     = int_argument_type_known,
            .value    = v.zpauto.offset,
            .zpauto   = true,
            .zp_scope = v.zpauto.scope,
            .zp_def   = v.zpauto.def,
            .zp_name  = v.zpauto.name
        };
    }

    if (value_is_error(v) && v.error.code == error_type_unknown_symbol) {
        if (final_pass) {
            return (int_argument) {
                .type = int_argument_type_error,
                .error = error_type_undefined_symbol,
                .error_at = at,
                .error_detail = v.error.detail   // the symbol's name rides along into the diagnostic
            };
        }
        return (int_argument) { .type = int_argument_type_unresolved };   // a forward reference; settles later
    }

    // Any other error value carries its specific cause through unchanged (value and the assembler share
    // one error_type space); a non-error non-number (a string, a list) is simply not an address.
    return (int_argument) {
        .type = int_argument_type_error,
        .error = value_is_error(v) ? v.error.code : error_type_operand_not_numeric,
        .error_at = at,
        .error_detail = value_is_error(v) ? v.error.detail : (rc_str) {0}
    };
}

// Refuse a ZPAUTO address in a context that needs a real number NOW - a count, a condition, a layout
// address: the address exists only after allocation, long after this decision must be made. Demotes the
// argument to the dedicated error, which the caller's ordinary error path then reports (self-gated on
// the final pass, at the use site, naming the variable).
static int_argument int_argument_no_zpauto(int_argument arg, uint32_t at)
{
    if (arg.zpauto) {
        return (int_argument) {
            .type         = int_argument_type_error,
            .error        = error_type_zpauto_address,
            .error_at     = at,
            .error_detail = arg.zp_name
        };
    }
    return arg;
}


// The two ways a parse reports an error - both record straight into b->diagnostics, so the code and
// location never travel in the parse_result. syntax_error unwinds (the stream is broken); semantic_error
// carries on (only recording on the settling pass of a live branch). Shared with opcodes.c.
parse_result syntax_error_payload(baron *b, error_type code, cursor at, rc_str payload)
{
    baron_error_payload(b, code, at, payload);
    return (parse_result) {.next = at.pos, .fatal = true};
}

parse_result syntax_error(baron *b, error_type code, cursor at)
{
    return syntax_error_payload(b, code, at, (rc_str) {0});
}

void semantic_error_payload(baron *b, parse_flags flags, error_type code, cursor at, rc_str payload)
{
    if (flags.final && flags.active) {
        baron_error_payload(b, code, at, payload);
    }
}

void semantic_error(baron *b, parse_flags flags, error_type code, cursor at)
{
    semantic_error_payload(b, flags, code, at, (rc_str) {0});
}

void semantic_warning(baron *b, parse_flags flags, error_type code, cursor at, uint8_t severity)
{
    if (flags.final && flags.active) {
        baron_warning(b, code, at, severity);
    }
}


// ---- the verbose listing ----

// Column layout: "  0900  AD 34 12        LDA magic" - a two-space margin, four hex digits of address,
// two spaces, a byte field wide enough for four "XX " triplets plus a truncation ellipsis, then the
// statement's source text. Everything hangs off these two numbers.
enum {
    verbose_max_bytes  = 4,    // hex bytes shown before the dump is truncated with "..."
    verbose_byte_field = 16,   // the byte field's total width including the gap before the source text
};

// Echo the statement's source [stmt.pos, end_pos) into the listing. Leading blanks are trimmed (a
// statement after a same-line label starts on the space that followed the name); everything else is
// kept verbatim, because relating what was WRITTEN to what assembled is the listing's whole point.
// Only a list literal may legally span lines, so a span containing a newline is cut at the first one
// and finished with an ellipsis.
static void verbose_source(baron *b, cursor stmt, uint32_t end_pos)
{
    rc_str text = source_files_text(&b->source_files, stmt.source);
    uint32_t begin = stmt.pos;
    while (begin < end_pos && (text.data[begin] == ' ' || text.data[begin] == '\t')) {
        begin++;
    }
    uint32_t end = begin;
    while (end < end_pos && text.data[end] != '\n') {
        end++;
    }
    bool cut = end < end_pos;
    if (cut) {
        // A multi-line literal's first line may end in blanks (or a CR); tidy them before the ellipsis.
        while (end > begin && (text.data[end - 1] == ' ' || text.data[end - 1] == '\t' ||
                               text.data[end - 1] == '\r')) {
            end--;
        }
    }
    rc_mstr_append(&b->channels[0], rc_str_substr(text, begin, end - begin), b->per_pass);
    if (cut) {
        rc_mstr_append(&b->channels[0], RC_STR("..."), b->per_pass);
    }
    rc_mstr_append_char(&b->channels[0], '\n', b->per_pass);
}

// The listing gate mirrors semantic_error's: only the listing pass of a live branch leaves a trace.
static bool verbose_on(parse_flags flags)
{
    return flags.listing && flags.active;
}

void verbose_code_line(baron *b, parse_flags flags, cursor stmt, uint32_t end_pos,
                       uint32_t section, uint32_t pc, uint32_t code_begin)
{
    if (!verbose_on(flags)) {
        return;
    }
    rc_mstr_append(&b->channels[0], RC_STR("  "), b->per_pass);
    rc_mstr_append_hex16(&b->channels[0], (uint16_t) pc, b->per_pass);
    rc_mstr_append(&b->channels[0], RC_STR("  "), b->per_pass);

    // The bytes are read back from the section rather than passed in: this runs on the listing pass,
    // after zero-page allocation, so what sits there IS the final output.
    rc_view_bytes code = sections_code(&b->sections, section);
    uint32_t num = code.num - code_begin;
    uint32_t shown = num > verbose_max_bytes ? verbose_max_bytes : num;
    uint32_t width = 0;
    for (uint32_t i = 0; i < shown; i++) {
        if (i > 0) {
            rc_mstr_append_char(&b->channels[0], ' ', b->per_pass);
        }
        rc_mstr_append_hex8(&b->channels[0], rc_view_bytes_get(code, code_begin + i), b->per_pass);
        width += (i > 0) ? 3 : 2;
    }
    if (num > verbose_max_bytes) {
        rc_mstr_append(&b->channels[0], RC_STR("..."), b->per_pass);
        width += 3;
    }
    rc_mstr_append_n(&b->channels[0], ' ', verbose_byte_field - width, b->per_pass);
    verbose_source(b, stmt, end_pos);
}

void verbose_text_line(baron *b, parse_flags flags, cursor stmt, uint32_t end_pos,
                       uint32_t pc, verbose_text_kind kind)
{
    if (!verbose_on(flags)) {
        return;
    }
    if (kind == verbose_text_address) {
        // An address but no bytes: the line marks where something lands (a macro expansion, an
        // included file) - the bytes belong to the statements that follow.
        rc_mstr_append(&b->channels[0], RC_STR("  "), b->per_pass);
        rc_mstr_append_hex16(&b->channels[0], (uint16_t) pc, b->per_pass);
        rc_mstr_append_n(&b->channels[0], ' ', 2 + verbose_byte_field, b->per_pass);
    }
    verbose_source(b, stmt, end_pos);
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
    closer_endbasic,
} closer_kind;

// Pre-combined: every mnemonic (its own lexeme type, carrying the id) plus the statement
// directives (ORG, the '.' label introducer, the '{' scope opener) and the block closers. '#' and
// '=' never start a statement, so they live in the operand / assignment tables instead
// (operand_tokens is in opcodes.c, next to the operand parser).
static const token statement_token_entries[] = {
    {RC_STR_INIT("adc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_adc}}},
    {RC_STR_INIT("and"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_and}}},
    {RC_STR_INIT("asl"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_asl}}},
    {RC_STR_INIT("bcc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bcc}}},
    {RC_STR_INIT("bcs"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bcs}}},
    {RC_STR_INIT("beq"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_beq}}},
    {RC_STR_INIT("bit"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bit}}},
    {RC_STR_INIT("bmi"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bmi}}},
    {RC_STR_INIT("bne"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bne}}},
    {RC_STR_INIT("bpl"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bpl}}},
    {RC_STR_INIT("brk"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_brk}}},
    {RC_STR_INIT("bvc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bvc}}},
    {RC_STR_INIT("bvs"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bvs}}},
    {RC_STR_INIT("clc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_clc}}},
    {RC_STR_INIT("cld"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cld}}},
    {RC_STR_INIT("cli"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cli}}},
    {RC_STR_INIT("clv"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_clv}}},
    {RC_STR_INIT("cmp"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cmp}}},
    {RC_STR_INIT("cpx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cpx}}},
    {RC_STR_INIT("cpy"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_cpy}}},
    {RC_STR_INIT("dec"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dec}}},
    {RC_STR_INIT("dex"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dex}}},
    {RC_STR_INIT("dey"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dey}}},
    {RC_STR_INIT("eor"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_eor}}},
    {RC_STR_INIT("inc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_inc}}},
    {RC_STR_INIT("inx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_inx}}},
    {RC_STR_INIT("iny"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_iny}}},
    {RC_STR_INIT("jmp"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_jmp}}},
    {RC_STR_INIT("jsr"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_jsr}}},
    {RC_STR_INIT("lda"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_lda}}},
    {RC_STR_INIT("ldx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ldx}}},
    {RC_STR_INIT("ldy"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ldy}}},
    {RC_STR_INIT("lsr"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_lsr}}},
    {RC_STR_INIT("nop"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_nop}}},
    {RC_STR_INIT("ora"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ora}}},
    {RC_STR_INIT("pha"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_pha}}},
    {RC_STR_INIT("php"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_php}}},
    {RC_STR_INIT("pla"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_pla}}},
    {RC_STR_INIT("plp"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_plp}}},
    {RC_STR_INIT("rol"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_rol}}},
    {RC_STR_INIT("ror"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ror}}},
    {RC_STR_INIT("rti"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_rti}}},
    {RC_STR_INIT("rts"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_rts}}},
    {RC_STR_INIT("sbc"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sbc}}},
    {RC_STR_INIT("sec"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sec}}},
    {RC_STR_INIT("sed"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sed}}},
    {RC_STR_INIT("sei"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sei}}},
    {RC_STR_INIT("sta"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sta}}},
    {RC_STR_INIT("stx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_stx}}},
    {RC_STR_INIT("sty"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_sty}}},
    {RC_STR_INIT("tax"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tax}}},
    {RC_STR_INIT("tay"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tay}}},
    {RC_STR_INIT("tsx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tsx}}},
    {RC_STR_INIT("txa"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_txa}}},
    {RC_STR_INIT("txs"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_txs}}},
    {RC_STR_INIT("tya"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tya}}},
    {RC_STR_INIT("bra"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_bra}}},
    {RC_STR_INIT("dea"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_dea}}},
    {RC_STR_INIT("ina"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ina}}},
    {RC_STR_INIT("phx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_phx}}},
    {RC_STR_INIT("phy"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_phy}}},
    {RC_STR_INIT("plx"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_plx}}},
    {RC_STR_INIT("ply"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_ply}}},
    {RC_STR_INIT("stz"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_stz}}},
    {RC_STR_INIT("clr"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_stz}}},
    {RC_STR_INIT("trb"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_trb}}},
    {RC_STR_INIT("tsb"), {.type = lexeme_type_opcode, .opcode = {.id = mnemonic_tsb}}},

    {RC_STR_INIT("."),      {.type = lexeme_type_keyword, .keyword = {.handle = handle_label}}},
    {RC_STR_INIT(".@"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_local_label}}},
    {RC_STR_INIT("{"),      {.type = lexeme_type_keyword, .keyword = {.handle = handle_open_brace}}},
    {RC_STR_INIT("skip"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_skip}}},
    {RC_STR_INIT("skipto"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_skipto}}},
    {RC_STR_INIT("align"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_align}}},
    {RC_STR_INIT("section"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_section}}},
    {RC_STR_INIT("zpreserve"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_zpreserve}}},
    {RC_STR_INIT("zpauto"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_zpauto_n}}},   // ZPAUTO <n>, <names>
    {RC_STR_INIT("zpauto1"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_zpauto1}}},   // 1-byte ZP variable
    {RC_STR_INIT("zpauto2"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_zpauto2}}},   // 2-byte ZP variable
    {RC_STR_INIT("unreachable"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_unreachable}}}, // dead fall-through
    {RC_STR_INIT("cancall"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_cancall}}},   // a JSR's real targets
    {RC_STR_INIT("canjump"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_canjump}}},   // a computed JMP's targets
    {RC_STR_INIT("discard"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_discard}}},   // a variable's value dies here
    {RC_STR_INIT("zpentry"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_zpentry}}},   // an external entry root
    {RC_STR_INIT("zpinterrupt"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_zpinterrupt}}}, // an interrupt handler root
    {RC_STR_INIT("equb"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equb}}},
    {RC_STR_INIT("equs"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equb}}},   // EQUS is an alias of EQUB
    {RC_STR_INIT("equw"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equw}}},   // 16-bit words
    {RC_STR_INIT("equd"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_equd}}},   // 32-bit words
    {RC_STR_INIT("if"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_if}}},
    {RC_STR_INIT("for"),    {.type = lexeme_type_keyword, .keyword = {.handle = handle_for}}},
    {RC_STR_INIT("include"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_include}}},
    {RC_STR_INIT("incbin"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_incbin}}},
    {RC_STR_INIT("incsection"), {.type = lexeme_type_keyword, .keyword = {.handle = handle_incsection}}},
    {RC_STR_INIT("basic"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_basic}}},   // inline BBC BASIC lines
    {RC_STR_INIT("macro"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_macro}}},
    {RC_STR_INIT("function"),{.type = lexeme_type_keyword, .keyword = {.handle = handle_function}}},
    {RC_STR_INIT("print"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_print}}},
    {RC_STR_INIT("error"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_error}}},
    // The pure expression constants are reserved at statement start too, so `pi = 5` is rejected rather than
    // quietly binding a shadowed symbol. Three near-identical rows, but it is only three tokens.
    {RC_STR_INIT("true"),   {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR_INIT("false"),  {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR_INIT("pi"),     {.type = lexeme_type_keyword, .keyword = {.handle = handle_reserved_constant}}},
    {RC_STR_INIT("elif"),   {.type = lexeme_type_closer, .closer = {closer_elif,  error_type_unexpected_elif}}},
    {RC_STR_INIT("else"),   {.type = lexeme_type_closer, .closer = {closer_else,  error_type_unexpected_else}}},
    {RC_STR_INIT("endif"),  {.type = lexeme_type_closer, .closer = {closer_endif, error_type_unexpected_endif}}},
    {RC_STR_INIT("next"),    {.type = lexeme_type_closer, .closer = {closer_next,     error_type_unexpected_next}}},
    {RC_STR_INIT("endmacro"),{.type = lexeme_type_closer, .closer = {closer_endmacro, error_type_unexpected_endmacro}}},
    {RC_STR_INIT("endsection"),{.type = lexeme_type_closer, .closer = {closer_endsection, error_type_unexpected_endsection}}},
    {RC_STR_INIT("endbasic"),{.type = lexeme_type_closer, .closer = {closer_endbasic, error_type_unexpected_endbasic}}},
    {RC_STR_INIT("}"),       {.type = lexeme_type_closer, .closer = {closer_brace,    error_type_unexpected_close_brace}}},
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
    {RC_STR_INIT("="), {.type = lexeme_type_assign}},
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
expr_result eval(baron *b, cursor at, uint32_t scope, uint32_t section, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    expr_env env = {
        .scopes         = &b->scopes,
        .scope_index    = scope,
        .pc             = sections_pc(&b->sections, section),
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
static parse_result handle_skip(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{

    expr_result e = eval(b, at, scope, section, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    uint32_t pc0   = sections_pc(&b->sections, section);
    uint32_t code0 = sections_code(&b->sections, section).num;

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_no_zpauto(int_argument_make(e.value, flags.final, at.pos), at.pos);
        switch (arg.type) {
            case int_argument_type_known:
                if (arg.value < 0) {
                    semantic_error(b, flags, error_type_skip_backwards, cursor_at(at, at.pos));   // skip nothing
                }
                else {
                    sections_skip(&b->sections, section, (uint32_t) arg.value);
                }
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown count emits nothing; forces another pass
                break;
            case int_argument_type_error:
                semantic_error_payload(b, flags, arg.error, cursor_at(at, arg.error_at), arg.error_detail);   // skip nothing
                break;
        }
    }
    verbose_code_line(b, flags, stmt, e.next, section, pc0, code0);

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// SKIPTO addr - pad with zeroes until pc reaches addr. Being already past addr is an error,
// deferred to the final pass since pc only settles once preceding forward references resolve.
static parse_result handle_skipto(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{

    expr_result e = eval(b, at, scope, section, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    uint32_t pc0   = sections_pc(&b->sections, section);
    uint32_t code0 = sections_code(&b->sections, section).num;

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_no_zpauto(int_argument_make(e.value, flags.final, at.pos), at.pos);
        switch (arg.type) {
            case int_argument_type_known: {
                uint32_t pc = sections_pc(&b->sections, section);
                if (arg.value < pc) {
                    semantic_error(b, flags, error_type_skip_backwards, cursor_at(at, at.pos));   // skip nothing
                }
                else {
                    sections_skip(&b->sections, section, (uint32_t) (arg.value - pc));
                }
                break;
            }
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown target emits nothing; forces another pass
                break;
            case int_argument_type_error:
                semantic_error_payload(b, flags, arg.error, cursor_at(at, arg.error_at), arg.error_detail);   // skip nothing
                break;
        }
    }
    verbose_code_line(b, flags, stmt, e.next, section, pc0, code0);

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// ALIGN n - pad with zeroes until pc is a multiple of n. n < 1 is meaningless (and would divide
// by zero), so it is an error; the modulo is only evaluated once we know n is sound.
static parse_result handle_align(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{

    expr_result e = eval(b, at, scope, section, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    uint32_t pc0   = sections_pc(&b->sections, section);
    uint32_t code0 = sections_code(&b->sections, section).num;

    bool unresolved = false;
    if (flags.active) {
        int_argument arg = int_argument_no_zpauto(int_argument_make(e.value, flags.final, at.pos), at.pos);
        switch (arg.type) {
            case int_argument_type_known:
                if (arg.value < 1) {
                    semantic_error(b, flags, error_type_bad_alignment, cursor_at(at, at.pos));   // pad nothing
                }
                else {
                    uint32_t n = (uint32_t) arg.value;
                    uint32_t rem = sections_pc(&b->sections, section) % n;
                    if (rem != 0) {
                        sections_skip(&b->sections, section, n - rem);
                    }
                }
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // an unknown alignment emits nothing; forces another pass
                break;
            case int_argument_type_error:
                semantic_error_payload(b, flags, arg.error, cursor_at(at, arg.error_at), arg.error_detail);   // pad nothing
                break;
        }
    }
    verbose_code_line(b, flags, stmt, e.next, section, pc0, code0);

    parse_result r = require_separator(b, cursor_at(at, e.next));
    r.unresolved = unresolved;
    return r;
}

// SECTION name, key = expr, ... / ENDSECTION - a lexically scoped region of object code, and its own address
// space. The name is UNIQUE (a repeat is error_type_duplicate_section: a name identifies one output blob, and
// there is no concatenation). The attributes are `key = expr` pairs resolved here: the assembler acts on `org`
// (this section's start address), `cmos` (65C02 encodings) and `guard` (the first address emission must not
// reach, checked when the block closes) and stores every attribute on the section for the output utility to
// read out of the result. `org` is an ordinary INHERITED attribute: a section with no `org` of its own starts at its
// parent's org (the default section's org is 0), and a section's emission never moves its parent's cursor -
// the two are separate spaces, so laying two sections at one address is fine (they never fall through into
// each other; only a control transfer that names a label crosses between them). A SECTION does NOT open a
// naming scope - labels inside bind in the enclosing scope, exactly as an IF body does. A dead branch parses
// the whole block for its extent but creates nothing and emits nothing.
static parse_result handle_section(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    // The name is a bare identifier (a mnemonic-spelled name would lex as that opcode - the same limitation
    // labels have, and not worth a private name table).
    lexer_result nm = lexer_next(src, at.pos, statement_tokens(b));
    if (nm.token.type != lexeme_type_identifier) {
        return syntax_error(b, error_type_expected_section_name, cursor_at(at, at.pos));
    }

    // Create the section (only when live). The parent section arrives as the threaded `section` parameter;
    // the child is passed on to the body's parse_block rather than stashed in a field. The cursor flows: the
    // child starts where the parent's pc sits (unless an `org` attribute jumps it), and on close the parent
    // resumes from the child's end.
    uint32_t child = RC_INDEX_NONE;
    if (flags.active) {
        child = sections_make(&b->sections, nm.token.identifier.name);
        if (child == RC_INDEX_NONE) {
            return syntax_error_payload(b, error_type_duplicate_section, cursor_at(at, at.pos),
                                        nm.token.identifier.name);   // names are unique
        }
        // Inherit the parent's whole attribute bag - the consumed keys included. org is just an inherited
        // attribute: a child with no `org` of its own starts at the parent's org (its BASE address), not
        // wherever the parent has emitted to; `cmos` likewise carries the parent's instruction set down.
        // Explicit attributes below override. A section's emission never moves its parent's cursor - the
        // two are separate address spaces.
        uint32_t parent_org   = 0;
        bool     parent_cmos  = false;
        uint32_t parent_guard = RC_INDEX_NONE;
        rc_view_attribute inherited = sections_attributes(&b->sections, section);
        for (uint32_t i = 0; i < inherited.num; i++) {
            attribute a = rc_view_attribute_get(inherited, i);
            sections_add_attribute(&b->sections, child, a.key, a.v, a.at);
            if (rc_str_is_equal_insensitive(a.key, RC_STR("org")) && value_is_number(a.v)) {
                parent_org = (uint32_t) ((int64_t) a.v.numeric & 0xFFFF);
            }
            else if (rc_str_is_equal_insensitive(a.key, RC_STR("cmos")) && value_is_number(a.v)) {
                parent_cmos = a.v.numeric != 0;
            }
            else if (rc_str_is_equal_insensitive(a.key, RC_STR("guard")) && value_is_number(a.v)) {
                parent_guard = (uint32_t) ((int64_t) a.v.numeric & 0xFFFF);
            }
        }
        sections_org(&b->sections, child, parent_org);
        sections_set_cmos(&b->sections, child, parent_cmos);
        sections_set_guard(&b->sections, child, parent_guard);
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
            return syntax_error(b, error_type_unexpected_token, cursor_at(at, comma.next));   // want an attribute name
        }
        lexer_result eq = lexer_next(src, key.next, assign_tokens);
        if (eq.token.type != lexeme_type_assign) {
            return syntax_error(b, error_type_expected_assign, cursor_at(at, key.next));
        }
        expr_result e = eval(b, cursor_at(at, eq.next), scope, section, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        if (child != RC_INDEX_NONE) {
            sections_add_attribute(&b->sections, child, key.token.identifier.name, e.value, cursor_at(at, comma.next));
            bool is_org   = rc_str_is_equal_insensitive(key.token.identifier.name, RC_STR("org"));
            bool is_cmos  = rc_str_is_equal_insensitive(key.token.identifier.name, RC_STR("cmos"));
            bool is_guard = rc_str_is_equal_insensitive(key.token.identifier.name, RC_STR("guard"));
            if (is_org || is_cmos || is_guard) {
                int_argument arg = int_argument_no_zpauto(int_argument_make(e.value, flags.final, eq.next), eq.next);
                switch (arg.type) {
                    case int_argument_type_known:
                        if (is_org) {
                            sections_org(&b->sections, child, (uint32_t) (arg.value & 0xFFFF));
                        }
                        else if (is_cmos) {
                            sections_set_cmos(&b->sections, child, arg.value != 0);
                        }
                        else {
                            sections_set_guard(&b->sections, child, (uint32_t) (arg.value & 0xFFFF));
                        }
                        break;
                    case int_argument_type_unresolved:
                        unresolved = true;   // a forward-referenced org/cmos owes another pass
                        break;
                    case int_argument_type_error:
                        semantic_error_payload(b, flags, arg.error, cursor_at(at, eq.next), arg.error_detail);
                        break;
                }
            }
        }
        pos = e.next;
    }

    // Close the SECTION line, parse the body up to ENDSECTION (in the child section), then thread the cursor
    // back so the parent resumes where the child left off.
    parse_result sep = require_separator(b, cursor_at(at, pos));
    if (sep.fatal) {
        return sep;
    }

    // The header line, at the margin like a label: a section frames what follows rather than landing
    // anywhere itself (its statements carry the addresses).
    if (child != RC_INDEX_NONE) {
        verbose_text_line(b, flags, stmt, pos, 0, verbose_text_margin);
    }

    // A dead branch has no child section: the body parses (for its extent) in the parent section, emitting
    // nothing.
    uint32_t body_section = (child != RC_INDEX_NONE) ? child : section;
    parse_result body = parse_block(b, cursor_at(at, sep.next), scope, body_section, flags, scratch);
    body.unresolved |= unresolved;

    // The parent's cursor is NOT touched by the child: sections are separate address spaces, so the parent
    // resumes exactly where it was before the SECTION, and the child's bytes never displace it.

    if (body.fatal) {
        return body;
    }

    lexer_result cl = lexer_next(src, body.next, statement_tokens(b));
    if (cl.token.type == lexeme_type_closer && cl.token.closer.id == closer_endsection) {
        if (child != RC_INDEX_NONE) {
            // The guard check: the section is complete (INCSECTION reservations included), so its pc
            // is the address just past the last byte - past the guard means the guarded address was
            // written. Recoverable: the report names the overshoot and assembly carries on.
            uint32_t guard = sections_guard(&b->sections, child);
            uint32_t pc    = sections_pc(&b->sections, child);
            if (guard != RC_INDEX_NONE && pc > guard) {
                char storage[16];
                rc_mstr over = {.data = storage, .len = 0, .cap = sizeof storage};
                rc_mstr_append_u32(&over, pc - guard, NULL);
                semantic_error_payload(b, flags, error_type_guard_exceeded, stmt, over.view);
            }
            // The ENDSECTION line at the margin too, and a blank line sets sections apart. Note the start
            // cursor is built here: `stmt` is the SECTION statement, not this closer.
            verbose_text_line(b, flags, cursor_at(at, body.next), cl.next, 0, verbose_text_margin);
            if (verbose_on(flags)) {
                rc_mstr_append_char(&b->channels[0], '\n', b->per_pass);
            }
        }
        body.next = cl.next;
        return fold(body, require_separator(b, cursor_at(at, body.next)));
    }
    return fold(body, syntax_error(b, error_type_unclosed_section, cursor_at(at, body.next)));   // a foreign closer / EOF
}

// BASIC ... ENDBASIC - an inline BBC BASIC program. Each numbered line is tokenised byte-for-byte as
// the BASIC 4 ROM would store it (basic.h has the algorithm) and emitted into the current section;
// ENDBASIC finishes the program with its 0D FF terminator. A line inside the block either starts
// with a decimal line number - the WHOLE line then belongs to the tokeniser, bypassing the lexer, so
// ':' and ';' are BASIC text there rather than Baron's separator and comment - or it is blank / a
// comment line / ENDBASIC; any other statement is refused (interspersing assembly among the lines is
// a possible later extension). The emitted bytes are pure text, identical every pass, so the block
// never disturbs convergence; a dead branch walks the lines to find its ENDBASIC and emits nothing.
static parse_result handle_basic(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) scope;
    rc_str src = source_files_text(&b->source_files, at.source);

    // BASIC takes no operands. The BASIC line at the margin, framing what follows like a SECTION.
    parse_result acc = require_separator(b, at);
    if (acc.fatal) {
        return acc;
    }
    verbose_text_line(b, flags, stmt, at.pos, 0, verbose_text_margin);

    while (true) {
        // A statement starting with a digit (past any indent or comment, the lexer's own notion of
        // blank space) is a BASIC line; nothing else in Baron begins with one, so the peek cannot
        // misfire. The peek comes before any lexing because the lexer must not see the line itself -
        // "10PRINT "A:B"" is one opaque span to Baron.
        uint32_t start = lexer_skip_whitespace(src, acc.next);
        if (start < src.len && src.data[start] >= '0' && src.data[start] <= '9') {
            uint32_t line_end   = lexer_line_end(src, start);
            cursor   line_stmt  = cursor_at(at, start);
            uint32_t pc0        = sections_pc(&b->sections, section);
            uint32_t code0      = sections_code(&b->sections, section).num;
            if (flags.active) {
                basic_line_result line = basic_tokenise_line(rc_str_substr(src, start, line_end - start), &scratch);
                if (line.error != error_type_none) {
                    // A bad line number / overlong line: recoverable, and skipping the record is
                    // pass-stable because the text never depends on a symbol.
                    semantic_error(b, flags, line.error, line_stmt);
                }
                for (uint32_t i = 0; i < line.bytes.num; i++) {
                    sections_emit_u8(&b->sections, section, rc_view_bytes_get(line.bytes, i));
                }
            }
            // The listing dumps the whole record from its 0D (a dropped erroring line shows an
            // empty byte field).
            verbose_code_line(b, flags, line_stmt, line_end, section, pc0, code0);
            acc.next = line_end;   // the newline is the next iteration's terminator lexeme
            continue;
        }

        lexer_result lr = lexer_next(src, acc.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_terminator) {
            if (lexer_at_end(src, lr.next)) {
                return fold(acc, syntax_error(b, error_type_unclosed_basic, cursor_at(at, acc.next)));
            }
            acc.next = lr.next;   // a blank or comment line between BASIC lines
            continue;
        }
        if (lr.token.type == lexeme_type_closer) {
            if (lr.token.closer.id != closer_endbasic) {
                return fold(acc, syntax_error(b, error_type_unclosed_basic, cursor_at(at, acc.next)));
            }
            uint32_t pc0   = sections_pc(&b->sections, section);
            uint32_t code0 = sections_code(&b->sections, section).num;
            if (flags.active) {
                sections_emit_u8(&b->sections, section, 0x0D);
                sections_emit_u8(&b->sections, section, 0xFF);   // "<cr> FF": the ROM's program-end marker
            }
            verbose_code_line(b, flags, cursor_at(at, acc.next), lr.next, section, pc0, code0);
            acc.next = lr.next;
            return fold(acc, require_separator(b, cursor_at(at, acc.next)));
        }
        return fold(acc, syntax_error(b, error_type_expected_basic_line, cursor_at(at, acc.next)));
    }
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
        semantic_error_payload(b, flags, arg.error, at, arg.error_detail);
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
static parse_result handle_zpreserve(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;

    if (flags.active) {
        zeropage_enable(&b->zeropage);   // ZPRESERVE turns the feature on even if the list is empty
    }

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
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

// The ZPAUTO name-binding worker, shared by ZPAUTO1 / ZPAUTO2 / ZPAUTO <n>: declare `width`-byte zero-page
// variables, auto-allocated from the ZPRESERVE set. Each name binds an ordinary scoped symbol (so `LDA foo`
// sizes as zero page and `routine.foo` resolves from outside, all for free) to a fixed PLACEHOLDER address;
// the real byte is assigned later, at the allocation phase. The variable's width + identity are recorded in
// the zeropage var registry, but only on the single final pass (the settling passes need just the placeholder
// binding for layout to converge). ZPAUTO is meaningless without a ZPRESERVE first: we flag that, but still
// bind the names so references do not cascade into undefined-symbol errors. A dead branch removes only the
// binding it owns, like a dead label.
static parse_result handle_zpauto(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, uint16_t width, rc_arena scratch)
{
    (void) stmt;      // the listing line is built from the allocated symbol, not the source echo
    (void) section;   // ZPAUTO binds a symbol; the section it sits in is recorded later, at the instruction site
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
            // The output pass leaves the binding ALONE: zeropage_finalize has already rewritten it to the
            // allocated address, which is exactly what re-emission must see. Re-binding the placeholder here
            // would put the un-allocated values back into the output.
            if (!flags.output) {
                symbol_status st = scopes_set_symbol(
                    &b->scopes, scope, name,
                    value_make_zpauto(scope, def, 0, name), def);

                if (st == symbol_status_duplicate) {
                    semantic_error_payload(b, flags, error_type_duplicate_symbol, def, name);
                    cursor original = scopes_symbol_def(&b->scopes, scope, name);
                    if (!cursor_is_none(original)) {
                        semantic_error_payload(b, flags, error_type_original_definition, original, name);
                    }
                }
                else if (flags.final) {
                    // Record the vreg. Its identity is the (scope, def) pair: a macro / FOR body shares one def
                    // across every instantiation, but each runs in its own child scope, so each instance becomes
                    // a distinct variable here - exactly as two sibling blocks declaring the same name would.
                    zeropage_add_var(&b->zeropage, name, scope, width, def);
                }
            }
            else if (flags.listing && cursor_is_equal(scopes_symbol_def(&b->scopes, scope, name), def)) {
                // ...and instead lists the assignment the declaration BECAME: the symbol now holds the
                // allocated byte, so `zpauto1 tmp` reads back as `tmp = &70 [auto]` - echoing the source
                // would only show a name with no address, and the address is the interesting part. The
                // binding must still be OURS, scope-locally: the allocator UNDEFINES an unused variable,
                // and without this check the lookup would walk up and print some outer namesake instead.
                value v = scopes_get_symbol(&b->scopes, scope, name);
                if (v.type == value_type_numeric) {
                    rc_mstr *out = &b->channels[0];
                    rc_mstr_append(out, name, b->per_pass);
                    rc_mstr_append(out, RC_STR(" = &"), b->per_pass);
                    rc_mstr_append_hex8(out, (uint8_t) v.numeric, b->per_pass);
                    rc_mstr_append(out, RC_STR(" [auto]\n"), b->per_pass);
                }
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

static parse_result handle_zpauto1(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_zpauto(b, stmt, at, scope, section, flags, 1, scratch);
}
static parse_result handle_zpauto2(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_zpauto(b, stmt, at, scope, section, flags, 2, scratch);
}

// ZPAUTO <count>, <names> - the generic form: variables <count> bytes wide (a table or struct), of which
// ZPAUTO1 / ZPAUTO2 are the 1- and 2-byte sugar. The count is a constant expression, evaluated here (a
// forward reference defers a pass, like any operand); it must land in 1..256 (the zero page). Then the
// name list is parsed by the shared worker, exactly as the fixed-width forms do.
static parse_result handle_zpauto_n(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = eval(b, at, scope, section, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }
    // A comma separates the count from the names: `ZPAUTO 4, table`.
    lexer_result comma = lexer_next(src, e.next, statement_tokens(b));
    if (comma.token.type != lexeme_type_comma) {
        return syntax_error(b, error_type_expected_var_name, cursor_at(at, e.next));
    }

    // Reduce the count. Unknown defers to a later pass; a known count must be a legal width; anything else is a
    // recoverable error. On any non-known outcome we fall back to width 1 so the names still bind (references
    // do not cascade), exactly as the feature-off / bad-name paths do.
    uint16_t width = 1;
    bool     unresolved = false;
    int_argument arg = int_argument_no_zpauto(int_argument_make(e.value, flags.final, at.pos), at.pos);
    if (arg.type == int_argument_type_unresolved) {
        unresolved = true;
    }
    else if (arg.type == int_argument_type_error) {
        semantic_error_payload(b, flags, arg.error, cursor_at(at, arg.error_at), arg.error_detail);
    }
    else if (arg.value < 1 || arg.value > (int64_t) zeropage_size) {
        semantic_error(b, flags, error_type_zpauto_bad_width, cursor_at(at, at.pos));
    }
    else {
        width = (uint16_t) arg.value;
    }

    parse_result r = handle_zpauto(b, stmt, cursor_at(at, comma.next), scope, section, flags, width, scratch);
    r.unresolved = r.unresolved || unresolved;
    return r;
}

// UNREACHABLE - a zero-byte assertion, placed right after an always-taken branch, that control cannot fall
// through to this point. The allocator's CFG would otherwise wire the branch's fall-through edge and treat
// whatever is live down that dead path as live across the branch, pinning bytes needlessly. Recording this
// pc lets zeropage_finalize prune that one edge. It is TRUSTED - a wrong UNREACHABLE (a fall-through that
// really can happen) is one of the few ways to defeat the certainty contract, but it is the programmer's
// explicit promise. Only meaningful on the final pass, and only with the feature enabled.
static parse_result handle_unreachable(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
    (void) scope;
    (void) scratch;
    if (flags.final && flags.active && zeropage_is_enabled(&b->zeropage)) {
        zeropage_add_cflow(&b->zeropage, (zp_cflow) {
            .site   = sections_pc(&b->sections, section),
            .target = RC_INDEX_NONE,
            .kind   = zp_cflow_unreachable,
            .at     = cursor_at(at, at.pos),
        });
    }
    return require_separator(b, at);
}

// The shared body of CANCALL / CANJUMP: parse a comma-separated list of target addresses and record one cflow
// of `kind` per target, sited on the last recorded instruction (the JSR / JMP / RTS this annotation
// qualifies) - but only when that instruction's flow fits the kind (a call for CANCALL; a jump OR a return
// for CANJUMP, the return being the RTS-dispatch trick - push a target address, RTS into it), so a stray
// CANCALL after a JMP (or vice versa) binds to nothing rather than mis-annotating. Only the final pass
// records instructions, so only then is there a site; the settling passes still parse the list so the
// statement stays well-formed. A forward target defers.
static parse_result handle_can_targets(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags,
                                       zp_cflow_kind kind, rc_arena scratch)
{
    (void) stmt;
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;

    uint32_t site = RC_INDEX_NONE;
    if (flags.final) {
        uint32_t ni = zeropage_insn_count(&b->zeropage);
        while (ni > 0 && zeropage_insn_get(&b->zeropage, ni - 1).var_kill) {
            ni--;   // a DISCARD between the instruction and its annotation is a marker, not the site
        }
        if (ni > 0) {
            zp_insn last = zeropage_insn_get(&b->zeropage, ni - 1);
            bool fits = (kind == zp_cflow_cancall)
                            ? last.flow == zp_flow_call
                            : last.flow == zp_flow_jump || last.flow == zp_flow_return;
            if (fits) {
                site = last.pc;
            }
        }
    }

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        if (flags.final && flags.active && zeropage_is_enabled(&b->zeropage) && site != RC_INDEX_NONE) {
            int_argument arg = int_argument_no_zpauto(int_argument_make(e.value, flags.final, pos), pos);
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
                    semantic_error_payload(b, flags, arg.error, cursor_at(at, arg.error_at), arg.error_detail);
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
// self-modified operand, or a dispatch the analysis cannot follow); with it, the callee footprint is bounded
// by the union of the named routines. TRUSTED, like UNREACHABLE. Note that a constant target off the
// assembled stream reads as a call OUT of the program (an external OS/ROM entry, empty footprint) - so a
// self-modified JSR whose placeholder operand is such a constant is NOT caught by the analysis, and the
// annotation is what makes it sound.
static parse_result handle_cancall(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_can_targets(b, stmt, at, scope, section, flags, zp_cflow_cancall, scratch);
}

// CANJUMP <targets> - the programmer declares the possible destinations of the computed / indirect JMP
// immediately preceding it (a jump table). Without it, the jump reaches code the CFG cannot follow and, with
// variables live, is refused (error_type_zpauto_computed_flow); with it, the CFG wires every named target as a
// real successor edge, so liveness follows control to each one. TRUSTED, like UNREACHABLE. Also legal after
// an RTS, for the dispatch trick (push a target address minus one, RTS into it): the annotated RTS is a jump
// in a return's clothing, and gets the declared edges instead of ending the routine.
static parse_result handle_canjump(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_can_targets(b, stmt, at, scope, section, flags, zp_cflow_canjump, scratch);
}

// DISCARD <var>[, <var>...] - the programmer's promise that the value each named ZPAUTO variable holds AT
// THIS POINT is never read again: everything read later comes from writes after here. Recorded as a size-0
// marker in the zp instruction stream; liveness treats it as a full-width kill that stores nothing, which is
// what lets an array rebuilt through indexed stores (`STA arr,X` - no provable byte written) have a live
// range that starts at its rebuild instead of leaking back to the routine entry and around the caller's
// loop. TRUSTED, like UNREACHABLE: a wrong DISCARD hands the variable's byte to someone else while the old
// value is still wanted. Whole variables only - the promise is hard enough to audit without byte windows.
static parse_result handle_discard(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;
    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }
        int_argument arg = int_argument_make(e.value, flags.final, pos);
        switch (arg.type) {
            case int_argument_type_known:
                if (!arg.zpauto || arg.value != 0) {
                    // A number, a fixed address, or a var+n slice: nothing here the allocator manages whole.
                    semantic_error_payload(b, flags, error_type_discard_needs_var, cursor_at(at, pos),
                                           arg.zpauto ? arg.zp_name : (rc_str) {0});
                }
                else if (flags.final && flags.active && zeropage_is_enabled(&b->zeropage)) {
                    zeropage_add_insn(&b->zeropage, (zp_insn) {
                        .pc           = sections_pc(&b->sections, section),
                        .size         = 0,
                        .flow         = zp_flow_normal,
                        .rw           = vref_none,
                        .vreg         = RC_INDEX_NONE,
                        .var_scope    = arg.zp_scope,
                        .var_def      = arg.zp_def,
                        .var_offset   = 0,
                        .var_kill     = true,
                        .target       = RC_INDEX_NONE,
                        .target_scope = RC_INDEX_NONE,
                        .target_def   = cursor_none(),
                        .section      = section,
                        .at           = cursor_at(at, pos),
                    });
                }
                break;
            case int_argument_type_unresolved:
                unresolved = true;   // a forward-declared variable; it binds on a later pass
                break;
            case int_argument_type_error:
            default:
                semantic_error_payload(b, flags, arg.error, cursor_at(at, arg.error_at), arg.error_detail);
                break;
        }
        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;
        }
        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;
    }
}

// The shared body of ZPENTRY / ZPINTERRUPT: a bare marker, like UNREACHABLE, recording the pc it stands at
// as a declared program entry. Nothing is emitted, so a marker just inside a routine records the same pc as
// its label - place it as the routine's first statement. zeropage_finalize turns the records into the
// reachability roots (and, for ZPINTERRUPT, the pinning of the handler's communication vars and footprint).
static parse_result handle_entry_mark(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags,
                                      bool interrupt, rc_arena scratch)
{
    (void) stmt;
    (void) scope;
    (void) scratch;
    if (flags.final && flags.active && zeropage_is_enabled(&b->zeropage)) {
        zeropage_add_entry(&b->zeropage, (zp_entry) {
            .section   = section,
            .pc        = sections_pc(&b->sections, section),
            .interrupt = interrupt,
            .at        = cursor_at(at, at.pos),
        });
    }
    return require_separator(b, at);
}

// ZPENTRY - marks the routine it opens as an external entry point (called from outside the program: a BASIC
// framework, another executable). Declared entries become the ONLY sync roots for the allocator's
// reachability check - one ZPENTRY anywhere replaces the default "each section's first instruction"
// presumption. Deliberately NOT an interface contract: an external API whose inputs/outputs matter should
// fix them to concrete addresses, not ZPAUTO them.
static parse_result handle_zpentry(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_entry_mark(b, stmt, at, scope, section, flags, false, scratch);
}

// ZPINTERRUPT - marks the routine it opens as an interrupt handler. An async root: besides feeding the
// reachability check, its communication vars (live-in at the handler) and its whole transitive footprint are
// pinned against the rest of the program, because the handler can preempt at any instruction.
static parse_result handle_zpinterrupt(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_entry_mark(b, stmt, at, scope, section, flags, true, scratch);
}

// Emit `bits` as `width` little-endian bytes into the current section.
static void emit_le(baron *b, uint32_t section, uint64_t bits, uint32_t width)
{
    for (uint32_t i = 0; i < width; i++) {
        sections_emit_u8(&b->sections, section, (uint8_t) (bits >> (8 * i)));
    }
}

// Emit one value into the current section as `width`-byte little-endian units, for EQUB/EQUW/EQUD (width
// 1/2/4). A string goes character by character (each char widened to `width` bytes); a range is enumerated;
// a list is descended (so nested lists and ranges flatten out); anything else is one `width`-byte unit via
// int_argument_make, where a forward reference emits a zero placeholder of the right size and asks for
// another pass. Returns .error/.error_at on failure and .unresolved when a value defers (its .next is
// unused). A dead branch emits nothing and raises nothing - mirroring inactive statements.
static parse_result emit_data(baron *b, uint32_t section, value v, parse_flags flags, cursor at, uint32_t width, rc_arena scratch)
{
    if (!flags.active) {
        return (parse_result) {0};
    }

    if (value_is_string(v)) {
        for (uint32_t i = 0; i < v.string.len; i++) {
            emit_le(b, section, (uint8_t) v.string.data[i], width);
        }
        return (parse_result) {0};
    }

    if (value_is_range(v)) {
        return emit_data(b, section, range_to_list(v.range, &scratch), flags, at, width, scratch);   // enumerated to a list (or an error leaf)
    }

    if (value_is_list(v)) {
        parse_result acc = {0};
        for (uint32_t i = 0; i < v.list.num; i++) {
            acc = fold(acc, emit_data(b, section, rc_view_value_get(v.list, i), flags, at, width, scratch));
        }
        return acc;
    }

    // A numeric, a forward reference, or some other error value - one `width`-byte unit either way.
    int_argument arg = int_argument_make(v, flags.final, at.pos);
    if (arg.type == int_argument_type_error) {
        semantic_error_payload(b, flags, arg.error, at, arg.error_detail);
        emit_le(b, section, 0, width);   // best-effort placeholder; keeps the size stable
        return (parse_result) {0};
    }
    if (arg.type == int_argument_type_unresolved) {
        emit_le(b, section, 0, width);   // placeholder of the right width; forces another pass
        return (parse_result) {.unresolved = true};
    }
    // We allow signed values, so the window is -(2^(8*width)-1) .. (2^(8*width)-1); recorded once settled.
    int64_t limit = (int64_t) ((1ull << (8 * width)) - 1);
    if (arg.value < -limit || arg.value > limit) {
        semantic_error(b, flags, error_type_value_out_of_range, at);
    }
    emit_le(b, section, (uint64_t) arg.value, width);
    return (parse_result) {0};
}

// EQUB / EQUS (width 1) / EQUW (2) / EQUD (4): a comma-separated list of values, each emitted as `width`-byte
// little-endian units (see emit_data). All take numbers, strings, ranges and lists alike; EQUS is an EQUB alias.
static parse_result handle_equ(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, uint32_t width, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    bool unresolved = false;
    uint32_t pc0   = sections_pc(&b->sections, section);
    uint32_t code0 = sections_code(&b->sections, section).num;

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        parse_result em = emit_data(b, section, e.value, flags, cursor_at(at, pos), width, scratch);
        if (em.fatal) {
            return em;
        }
        unresolved |= em.unresolved;

        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        verbose_code_line(b, flags, stmt, e.next, section, pc0, code0);   // one line spans the whole list

        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;   // end of the list: a terminator or '}'
    }
}

// The width-specialised entry points named in the statement table. EQUS is an alias of EQUB.
static parse_result handle_equb(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_equ(b, stmt, at, scope, section, flags, 1, scratch);
}
static parse_result handle_equw(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_equ(b, stmt, at, scope, section, flags, 2, scratch);
}
static parse_result handle_equd(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    return handle_equ(b, stmt, at, scope, section, flags, 4, scratch);
}

// The '#' that may introduce a PRINT channel. It is not a statement token ('#' never starts a
// statement), so PRINT peeks with its own tiny table - the same trick as assign_tokens.
static const token print_token_entries[] = {
    {RC_STR_INIT("#"), {.type = lexeme_type_hash}},
};
static const token_table print_tokens = RC_VIEW(print_token_entries);

// True on the one pass whose PRINT output the caller receives: the output pass when one runs (under
// -v the text lands in channel 0 between the listing lines being built there, and a ZPAUTO symbol
// prints its ALLOCATED address - during the settling passes it is not a number at all), the final
// pass otherwise. Everything has settled by either, so each PRINT speaks exactly once.
static bool print_on(const baron *b, parse_flags flags)
{
    bool extra_pass = b->want_verbose || zeropage_is_enabled(&b->zeropage);
    return flags.active && (extra_pass ? flags.output : flags.final);
}

// Append one PRINT value to its channel: strings raw (concatenation is the point - spacing belongs to
// the writer), everything else in value_format's diagnostic form (decimal numbers, {..} lists with
// nested strings quoted - just what a debug dump wants).
static void print_value(baron *b, uint32_t channel, value v)
{
    if (value_is_string(v)) {
        rc_mstr_append(&b->channels[channel], v.string, b->per_pass);
    }
    else {
        value_format(&b->channels[channel], v, b->per_pass);
    }
}

// PRINT [#n,] value [, value...] - write the values, concatenated, to output channel n (a single digit;
// 0 when no #n is given), one newline at the end. Channel 0 reaches stdout by default and the CLI can
// redirect any channel to a file (-logN). A PRINT with no values is a blank line. Output happens on one
// pass only (see print_on); a PRINT is not echoed in the -v listing - like an assignment it emits no
// bytes, and its output is its own trace.
static parse_result handle_print(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;
    uint32_t channel = 0;

    // The optional channel: '#', a single digit, and a comma - anything else after the '#' is malformed.
    lexer_result hash = lexer_next(src, pos, print_tokens);
    if (hash.token.type == lexeme_type_hash) {
        lexer_result num = lexer_next(src, hash.next, print_tokens);
        double n = num.token.numeric_literal.value;
        if (num.token.type != lexeme_type_numeric_literal || n != (double) (uint32_t) n || n >= baron_num_channels) {
            return syntax_error(b, error_type_bad_print_channel, cursor_at(at, hash.next));
        }
        lexer_result comma = lexer_next(src, num.next, print_tokens);
        if (comma.token.type != lexeme_type_comma) {
            return syntax_error(b, error_type_bad_print_channel, cursor_at(at, num.next));
        }
        channel = (uint32_t) n;
        pos = comma.next;
    }

    // Nothing (or nothing after the channel) is a blank line; require_separator re-reads the terminator.
    lexer_result peek = lexer_next(src, pos, statement_tokens(b));
    if (peek.token.type == lexeme_type_terminator ||
        (peek.token.type == lexeme_type_closer && peek.token.closer.id == closer_brace)) {
        if (print_on(b, flags)) {
            rc_mstr_append_char(&b->channels[channel], '\n', b->per_pass);
        }
        return require_separator(b, cursor_at(at, pos));
    }

    bool unresolved = false;
    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        if (value_is_error(e.value)) {
            if (e.value.error.code == error_type_unknown_symbol && !flags.final) {
                unresolved = true;   // a forward reference; it prints once everything settles
            }
            else {
                // The undefined-on-final promotion, matching int_argument_make; any other error value
                // carries its own cause through. semantic_error self-gates on the settling pass.
                error_type code = e.value.error.code == error_type_unknown_symbol
                                ? error_type_undefined_symbol : e.value.error.code;
                semantic_error_payload(b, flags, code, cursor_at(at, pos), e.value.error.detail);
            }
        }
        else if (print_on(b, flags)) {
            print_value(b, channel, e.value);
        }

        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        if (print_on(b, flags)) {
            rc_mstr_append_char(&b->channels[channel], '\n', b->per_pass);
        }
        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;
    }
}

// ERROR [value[, value...]] - the user's own diagnostic: the values are formatted exactly as PRINT would
// show them (strings raw, everything else in value_format's shape, concatenated with no separator) and
// recorded as a RECOVERABLE error at the statement. Recoverable because the statement is syntactically
// fine: parsing carries on, so further errors still accumulate, and the recorded severity_error fails
// the assemble at the end like any other. The message renders whole through error_type_user_error's bare
// "%" template, so a '%' inside it is inert. A forward reference in a value defers (unresolved) and
// settles like PRINT's; still unknown on the final pass it is the usual undefined symbol and the ERROR
// itself stays silent (its message could not be formed). A dead branch records nothing.
static parse_result handle_error(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    uint32_t pos = at.pos;

    // A bare ERROR still fires - there just is nothing to say beyond the location.
    lexer_result peek = lexer_next(src, pos, statement_tokens(b));
    if (peek.token.type == lexeme_type_terminator ||
        (peek.token.type == lexeme_type_closer && peek.token.closer.id == closer_brace)) {
        semantic_error(b, flags, error_type_user_error, stmt);
        return require_separator(b, cursor_at(at, pos));
    }

    // The message is only materialised on the pass that will record it (semantic_error's gate); the other
    // passes still evaluate every value, to defer on a forward reference.
    bool record = flags.final && flags.active;
    bool broken = false;   // a value errored: the undefined/original cause is recorded instead
    bool unresolved = false;
    rc_mstr msg = {0};

    while (true) {
        expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
        if (e.error != expr_error_none) {
            return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
        }

        if (value_is_error(e.value)) {
            broken = true;
            if (e.value.error.code == error_type_unknown_symbol && !flags.final) {
                unresolved = true;   // a forward reference; the message forms once everything settles
            }
            else {
                error_type code = e.value.error.code == error_type_unknown_symbol
                                ? error_type_undefined_symbol : e.value.error.code;
                semantic_error_payload(b, flags, code, cursor_at(at, pos), e.value.error.detail);
            }
        }
        else if (record) {
            if (value_is_string(e.value)) {
                rc_mstr_append(&msg, e.value.string, &scratch);
            }
            else {
                value_format(&msg, e.value, &scratch);
            }
        }

        lexer_result lr = lexer_next(src, e.next, statement_tokens(b));
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // another value follows
        }

        if (record && !broken) {
            semantic_error_payload(b, flags, error_type_user_error, stmt, msg.view);
        }
        parse_result r = require_separator(b, cursor_at(at, e.next));
        r.unresolved = unresolved;
        return r;
    }
}

static parse_result handle_label(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
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
            value_make_numeric((double)sections_pc(&b->sections, section)),
            at
        );

        if (st == symbol_status_duplicate) {
            semantic_error_payload(b, flags, error_type_duplicate_symbol, cursor_at(at, at.pos), name);
            cursor original = scopes_symbol_def(&b->scopes, scope, name);
            if (!cursor_is_none(original)) {
                semantic_error_payload(b, flags, error_type_original_definition, original, name);   // point at the first binding
            }
        }
        r.changed = (st == symbol_status_changed);

        // Record a marker tying this label's identity (scope, def=at - the pair a reference resolves to) to
        // its placement, so a JSR/JMP/branch that names it finds the right block even where banks share the
        // address. Final pass only, feature on - like the insn / annotation streams.
        if (flags.final && zeropage_is_enabled(&b->zeropage)) {
            zeropage_add_label(&b->zeropage, scope, at, section, sections_pc(&b->sections, section));
        }
    }
    else if (cursor_is_equal(scopes_symbol_def(&b->scopes, scope, name), at)) {
        // Dead branch: clear only the binding THIS label owns (its def is our cursor), so a live sibling
        // branch's - or an outer statement's - like-named binding survives.
        r.changed = scopes_remove_symbol(&b->scopes, scope, name);
    }

    verbose_text_line(b, flags, stmt, nm.next, 0, verbose_text_margin);   // ".name" at the margin (never its scope's braces)

    // What follows decides the label's shape. A '{' - optionally one separator away, so it may sit
    // on the next line - makes the label name a scope. Anything else is not the label's to parse: we
    // drop back to the parent loop, which takes the next token as its own statement (or, on a '}',
    // closes the block). The label never owns the statement that follows it.
    lexer_result nb = lexer_next(src, r.next, statement_tokens(b));
    uint32_t brace_start = r.next;

    if (nb.token.type == lexeme_type_terminator) {
        lexer_result after = lexer_next(src, nb.next, statement_tokens(b));
        if (!is_open_brace(after.token)) {
            return r;                          // stand-alone label - leave the terminator for the loop
        }
        brace_start = nb.next;                 // .label <separator> { ... } still names the scope
        nb = after;
    }

    if (is_open_brace(nb.token)) {
        verbose_text_line(b, flags, cursor_at(at, brace_start), nb.next, 0, verbose_text_margin);
        uint32_t child = scopes_get_or_make_child(&b->scopes, scope, name);
        return fold(r, parse_scope(b, cursor_at(at, nb.next), child, section, flags, scratch));
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
static parse_result handle_local_label(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
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
            value_make_numeric((double) sections_pc(&b->sections, section)),
            at);
        r.changed = (st == symbol_status_changed);   // a moved local label drives another pass, like any label
    }
    else if (cursor_is_equal(scopes_symbol_def(&b->scopes, scope, key.view), at)) {
        // Dead branch: clear only the binding this local label owns. Its @source:pos key is unique per
        // position, so the guard is a no-op here, but we keep it uniform with the named-label / assignment cases.
        r.changed = scopes_remove_symbol(&b->scopes, scope, key.view);
    }
    verbose_text_line(b, flags, stmt, at.pos, 0, verbose_text_margin);   // the ".@" token is the whole statement
    return r;
}

static parse_result handle_open_brace(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    verbose_text_line(b, flags, stmt, at.pos, 0, verbose_text_margin);   // '{' at the margin, like a label
    // The just-passed pos gives the anonymous scope a stable per-pass identity, so re-walking it
    // on a later pass keeps the same bindings.
    char storage[64];
    rc_mstr key = anon_scope_key(storage, sizeof storage, at);
    uint32_t child = scopes_get_or_make_child(&b->scopes, scope, key.view);
    return parse_scope(b, at, child, section, flags, scratch);
}

// IF cond ... ELIF cond ... ELSE ... ENDIF, evaluated each pass. IF does not open a scope: labels in
// the live branch leak to the enclosing scope, and the construct must close (ENDIF) inside the same
// scope it opened in. ELIF is just an IF in the else position (IF a x ELIF b y ELSE z == IF a x ELSE
// {IF b y ELSE z}), so this reads that way: one condition picks between a true body, active iff
// flags.active && the condition holds, and an else body, active iff flags.active && the condition is
// known FALSE. At most one is live - and neither when the condition cannot yet be evaluated, which
// owes another pass (a hard error on the final pass). Entered just past the IF - or, via the
// recursion, the ELIF - at the condition.
static parse_result handle_if(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    expr_result e = eval(b, at, scope, section, scratch);
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
        int_argument cond = int_argument_no_zpauto(int_argument_make(e.value, flags.final, at.pos), at.pos);
        switch (cond.type) {
            case int_argument_type_known:
                if_cond = (cond.value != 0);
                else_cond = !if_cond;
                break;
            case int_argument_type_unresolved:
                acc.unresolved = true;   // undecidable yet; owe another pass
                break;
            case int_argument_type_error:
                semantic_error_payload(b, flags, cond.error, cursor_at(at, cond.error_at), cond.error_detail);   // neither branch runs
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
            section,
            (parse_flags) {.final = flags.final, .active = flags.active && if_cond, .output = flags.output, .listing = flags.listing},
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
                stmt,   // the chain shares the IF statement's start (unused - IF lines are not listed)
                cursor_at(at, t.next),
                scope,
                section,
                (parse_flags) {.final = flags.final, .active = flags.active && else_cond, .output = flags.output, .listing = flags.listing},
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
                section,
                (parse_flags) {.final = flags.final, .active = flags.active && else_cond, .output = flags.output, .listing = flags.listing},
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
static parse_result handle_assignment(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_str name, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);

    lexer_result eq = lexer_next(src, at.pos, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return syntax_error(b, error_type_expected_assign, cursor_at(at, at.pos));   // not an assignment: malformed
    }

    expr_result e = eval(b, cursor_at(at, eq.next), scope, section, scratch);
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
            semantic_error_payload(b, flags, error_type_duplicate_symbol, cursor_at(at, at.pos), name);
            cursor original = scopes_symbol_def(&b->scopes, scope, name);
            if (!cursor_is_none(original)) {
                semantic_error_payload(b, flags, error_type_original_definition, original, name);   // point at the first binding
            }
        }
        else {
            if (value_is_error(e.value)) {
                if (e.value.error.code == error_type_unknown_symbol) {
                    semantic_error_payload(b, flags, error_type_undefined_symbol, cursor_at(at, eq.next),
                                           e.value.error.detail);
                    if (!flags.final) r.unresolved = true;   // a forward reference in the value; settles on a later pass
                }
                else {
                    semantic_error_payload(b, flags, e.value.error.code, cursor_at(at, eq.next),
                                           e.value.error.detail);   // e.g. x = 1/0
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

    // An assignment binds a symbol and emits nothing, so it echoes at the margin like a label - the
    // reader can follow the symbols without mistaking them for code.
    verbose_text_line(b, flags, stmt, e.next, 0, verbose_text_margin);

    return fold(r, require_separator(b, cursor_at(at, e.next)));
}


// ---- the FOR loop ----

// FOR <var> = <range-or-list> : ... : NEXT, evaluated each pass. Each iteration runs the body once with
// <var> bound to that element in its own per-iteration child scope, so body labels never collide across
// iterations. An empty sequence (e.g. {} or 5..<5) runs the body once inactive - zero bytes, no error.
// A sequence whose count is not yet known (a forward reference) does the same and forces another pass;
// on the final pass that is a hard undefined_symbol. Like IF, FOR opens no scope of its own for the loop
// control (only the per-iteration body scopes) and must close (NEXT) inside the scope it began in.
static parse_result handle_for(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
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

    expr_result e = eval(b, cursor_at(at, eq.next), scope, section, scratch);
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
            if (seq.error.code == error_type_unknown_symbol) {
                semantic_error_payload(b, flags, error_type_undefined_symbol, cursor_at(at, eq.next),
                                       seq.error.detail);
                if (!flags.final) unresolved = true;   // the count is not known yet; defer and try again next pass
            }
            else {
                semantic_error_payload(b, flags, seq.error.code, cursor_at(at, eq.next),
                                       seq.error.detail);   // e.g. the sequence expression divided by zero
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
        acc = fold(acc, parse_block(b, cursor_at(at, body_start), child, section,
                                    (parse_flags) {.final = flags.final, .active = active_body, .output = flags.output, .listing = flags.listing}, scratch));
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
static parse_result handle_include(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{

    // The filename is a string operand, evaluated on the spot - we load the file this very pass, so a
    // forward-referenced name is no use to us (it stays an error value, and we grumble about it below).
    expr_result e = eval(b, at, scope, section, scratch);
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
                    // The INCLUDE line itself: address, no bytes - the spliced file's lines follow.
                    verbose_text_line(b, flags, stmt, e.next, sections_pc(&b->sections, section), verbose_text_address);
                    uint32_t errors_before = baron_error_count(b);
                    b->include_depth++;
                    pulled = parse_file(b, (cursor) {.source = inc_source, .pos = 0}, scope, section, flags, scratch);
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
        else if (value_is_error(e.value) && e.value.error.code == error_type_unknown_symbol) {
            // A forward-referenced filename: defer, exactly like a forward address. We pull in nothing this
            // pass; once the name binds on a later pass the string resolves and the file loads. Still unknown
            // on the final pass means it never will be (a name defined only inside the file it would name), so
            // we call it out then.
            if (flags.final) {
                semantic_error_payload(b, flags, error_type_undefined_symbol, cursor_at(at, at.pos),
                                       e.value.error.detail);
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
static parse_result handle_incbin(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    expr_result e = eval(b, at, scope, section, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, cursor_at(at, e.error_at));
    }

    uint32_t pc0   = sections_pc(&b->sections, section);
    uint32_t code0 = sections_code(&b->sections, section).num;

    parse_result pulled = {0};
    if (flags.active) {
        if (value_is_string(e.value)) {
            rc_str base = source_files_name(&b->source_files, at.source);
            rc_str path = file_path_resolve(base, e.value.string, &scratch);
            if (flags.final || flags.output) {
                // Final pass: load the file and emit its bytes for real (one byte at a time, via emit_data).
                // The listing pass repeats this - its sections are the ones the result carries, so it must
                // hold the real bytes too (and the listing's hex dump reads them back).
                rc_file_load_binary_result f = rc_file_load_binary(path, 0, &scratch);
                if (f.error != RC_FILE_OK) {
                    return syntax_error(b, error_type_source_load, cursor_at(at, at.pos));
                }
                rc_str bytes = {.data = (const char *) f.contents.view.data, .len = f.contents.view.num};
                pulled = emit_data(b, section, value_make_string(bytes), flags, cursor_at(at, at.pos), 1, scratch);
            }
            else {
                // Settling passes: reserve the file's size without reading it, so pc / later labels are right.
                rc_file_size_result sz = rc_file_size(path);
                if (sz.error != RC_FILE_OK) {
                    return syntax_error(b, error_type_source_load, cursor_at(at, at.pos));
                }
                sections_skip(&b->sections, section, sz.size);
            }
        }
        else if (value_is_error(e.value) && e.value.error.code == error_type_unknown_symbol) {
            // A forward-referenced filename: defer, like INCLUDE. Still unknown on the final pass means it never
            // binds (a name defined only where it cannot be seen in time), so we call it out then.
            if (flags.final) {
                semantic_error_payload(b, flags, error_type_undefined_symbol, cursor_at(at, at.pos),
                                       e.value.error.detail);
            }
            else {
                pulled.unresolved = true;
            }
        }
        else {
            semantic_error(b, flags, error_type_expected_filename, cursor_at(at, at.pos));
        }
    }
    verbose_code_line(b, flags, stmt, e.next, section, pc0, code0);

    return fold(pulled, require_separator(b, cursor_at(at, e.next)));
}

// INCSECTION <name> - splice the assembled bytes of the named section here: INCBIN, but sourced from a
// section's code buffer. Only SPACE is reserved during the passes (the source's best-known size, so the
// layout settles whatever order the sections appear in); the bytes themselves land in one final,
// dependency-ordered fixup step AFTER the zero-page allocator has patched them (splices_resolve).
// Consequences: the section may be defined LATER in the source; a missing one is judged only on the final
// settled state; a circular arrangement is refused the first pass it is seen; and the -v listing shows an
// address-only line (the bytes belong to the fixup, not to this statement). The copy is literal - no
// relocation - which is the point: the bytes are meant to run at the SOURCE section's addresses once the
// program has moved them there.
static parse_result handle_incsection(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) scope;
    (void) scratch;
    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result nm = lexer_next(src, at.pos, statement_tokens(b));
    if (nm.token.type != lexeme_type_identifier) {
        return syntax_error(b, error_type_expected_section_name, cursor_at(at, at.pos));
    }
    uint32_t pc0 = sections_pc(&b->sections, section);
    if (flags.active) {
        sections_splice(&b->sections, section, nm.token.identifier.name, cursor_at(at, at.pos));
    }
    verbose_text_line(b, flags, stmt, nm.next, pc0, verbose_text_address);
    return require_separator(b, cursor_at(at, nm.next));
}


// ---- MACRO ----

// MACRO name [signature] : ...body... : ENDMACRO, evaluated each pass. The name/token is registered BEFORE
// the body is scanned, so the body may call the macro itself (self-recursion); mutual recursion needs a
// forward declaration - an empty body - so the partner's name is a token in time. The body is captured as a
// cursor and only ever PARSED at invocation; here we scan it inactively (the trick FOR uses to locate NEXT),
// which finds ENDMACRO through the real parser - handling multi-line list literals and nested IF/FOR/{} -
// without expanding anything. The definition emits nothing itself.
static parse_result handle_macro(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
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
    parse_result scan = parse_block(b, body, scope, section, (parse_flags) {.final = flags.final, .active = false, .output = flags.output, .listing = flags.listing}, scratch);
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
    {RC_STR_INIT("("), {.type = lexeme_type_open_paren}},
    {RC_STR_INIT(")"), {.type = lexeme_type_close_paren}},
};
static const token_table func_paren_tokens = RC_VIEW(func_paren_entries);

// FUNCTION name(params) [ : body ] = return-expr, evaluated each pass. The name/token is registered BEFORE
// the body is scanned, so the body may call the function itself (self-recursion). The definition is a
// STATEMENT (it emits nothing); the body's value semantics live in the evaluator (expression.c), which we
// call here only to locate the top-level '=' return and learn whether this is a real body or a forward
// declaration (an empty body plus an empty return expression).
static parse_result handle_function(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
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
        .pc             = sections_pc(&b->sections, section),
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
                            uint32_t scope, uint32_t section, uint32_t *end, rc_arena scratch)
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
            expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
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
static parse_result handle_macro_invocation(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, uint32_t macro_index, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    macro *m = macros_at(&b->macros, macro_index);

    // Match an overload: sorted so tokens beat expressions, first full match wins. Matching runs whether or
    // not we are active - it validates the arguments and, above all, advances the cursor past the call.
    uint32_t chosen = RC_INDEX_NONE;
    uint32_t args_end = at.pos;
    for (uint32_t si = 0; si < m->signatures.num; si++) {
        uint32_t end;
        if (macro_try_match(b, src, at, m, rc_array_macro_signature_get(&m->signatures, si), scope, section, &end, scratch)) {
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
            expr_result e = eval(b, cursor_at(at, pos), scope, section, scratch);
            scopes_set_symbol(&b->scopes, child, slot.name, e.value, at);
            pos = e.next;
        }
    }

    // The invocation itself: address, no bytes - the expansion's own lines follow with the bytes.
    verbose_text_line(b, flags, stmt, args_end, sections_pc(&b->sections, section), verbose_text_address);

    // Expand the body in the child scope. Its errors point at the definition (parsed in place); if it raised
    // any, drop an "expanded from here" breadcrumb at the call site - the handle_include idiom.
    uint32_t errors_before = baron_error_count(b);
    b->macro_depth++;
    parse_result bodyr = parse_block(b, body, child, section, flags, scratch);
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
static parse_result handle_reserved_constant(baron *b, cursor stmt, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    (void) stmt;
    (void) scope;
    (void) section;
    (void) flags;
    (void) scratch;
    return syntax_error(b, error_type_reserved_constant, cursor_at(at, at.pos));
}


// ---- the statement loop ----

static parse_result parse_one_statement(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, at.pos, statement_tokens(b));

    // `at` is the statement's true start; the handlers get it as `stmt` (for source echoing) alongside
    // the post-token cursor they parse from.
    switch (lr.token.type) {
        case lexeme_type_opcode:
            return opcode_parse(b, (mnemonic)lr.token.opcode.id, at, cursor_at(at, lr.next), scope, section, flags, scratch);
        case lexeme_type_keyword:
            return lr.token.keyword.handle(b, at, cursor_at(at, lr.next), scope, section, flags, scratch);
        case lexeme_type_macro:
            return handle_macro_invocation(b, at, cursor_at(at, lr.next), scope, section, flags, lr.token.macro.index, scratch);
        case lexeme_type_identifier:
            return handle_assignment(b, at, cursor_at(at, lr.next), scope, section, flags, lr.token.identifier.name, scratch);
        default:
            return syntax_error(b, error_type_unexpected_token, at);
    }
}

// Parse statements until the block's closer: a '}' or end of input. It stops AT the closer -
// leaving a '}' unconsumed - and treats neither as an error, because which closer is required
// depends on the caller: parse_scope wants the '}', run_pass wants end of input.
static parse_result parse_block(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
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

        acc = fold(acc, parse_one_statement(b, cursor_at(at, acc.next), scope, section, flags, scratch));
    }
    return acc;
}

// A braced block: parse its statements (entered just past the '{') and require the closing '}';
// reaching end of input first is an unclosed scope.
static parse_result parse_scope(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    parse_result r = parse_block(b, at, scope, section, flags, scratch);

    if (r.fatal) {
        return r;
    }

    rc_str src = source_files_text(&b->source_files, at.source);
    lexer_result lr = lexer_next(src, r.next, statement_tokens(b));

    if (lr.token.type == lexeme_type_closer) {
        if (lr.token.closer.id == closer_brace) {   // the '}' we were waiting for
            verbose_text_line(b, flags, cursor_at(at, r.next), lr.next, 0, verbose_text_margin);
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
static parse_result parse_file(baron *b, cursor at, uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch)
{
    parse_result r = parse_block(b, at, scope, section, flags, scratch);

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

// The INCSECTION dependency walk, shared by the per-pass cycle check and the final fixup. Splices release
// in "everything spliced INTO my source has landed first" order - which is exactly the order the copies
// must run in, so a chain (A inserts B inserts C) carries C's bytes through B into A - and a splice that
// never releases sits on (or behind) a dependency cycle. With `apply` set this is the real fixup: an
// unknown source is an error (every IF arm has settled by now, so absence is final) and each released
// splice copies its source's bytes over the span it reserved. Without it, the walk only proves
// acyclicity: an unknown source may yet appear on a later pass, so it blocks nothing and errors nothing.
// Returns false with diagnostics recorded when anything refused.
static bool splices_resolve(baron *b, bool apply, rc_arena scratch)
{
    rc_view_splice all = sections_splices(&b->sections);
    if (all.num == 0) {
        return true;
    }

    uint32_t *src  = rc_arena_alloc_type(&scratch, uint32_t, all.num);
    bool     *done = rc_arena_alloc_zero_type(&scratch, bool, all.num);
    uint32_t remaining = all.num;
    bool ok = true;
    for (uint32_t i = 0; i < all.num; i++) {
        splice sp = rc_view_splice_get(all, i);
        src[i] = sections_find(&b->sections, sp.src_name);
        if (src[i] == RC_INDEX_NONE) {
            if (apply) {
                baron_error_payload(b, error_type_unknown_section, sp.at, sp.src_name);
                ok = false;
            }
            done[i] = true;   // absent: it blocks nothing (and cannot sit on a cycle)
            remaining--;
        }
    }
    if (!ok) {
        return false;
    }

    while (remaining > 0) {
        bool progress = false;
        for (uint32_t i = 0; i < all.num; i++) {
            if (done[i]) {
                continue;
            }
            bool ready = true;   // ready iff nothing unapplied still splices INTO our source
            for (uint32_t j = 0; j < all.num && ready; j++) {
                ready = done[j] || rc_view_splice_get(all, j).dst != src[i];
            }
            if (ready) {
                if (apply) {
                    splice sp = rc_view_splice_get(all, i);
                    sections_copy_in(&b->sections, sp.dst, sp.dst_offset, src[i]);
                }
                done[i] = true;
                remaining--;
                progress = true;
            }
        }
        if (!progress) {
            for (uint32_t i = 0; i < all.num; i++) {
                if (!done[i]) {
                    splice sp = rc_view_splice_get(all, i);
                    baron_error_payload(b, error_type_circular_incsection, sp.at, sp.src_name);
                }
            }
            return false;
        }
    }
    return true;
}


// ---- command-line predefines ----

// Apply one -D "name=expression" definition into the root scope. The string is registered as a tiny
// synthetic source named after the switch itself, so a diagnostic points somewhere readable
// ("-D DEBUG=TRU:1:9: ..."); registration is keyed by name, so every pass lands on the same source
// index and the binding's identity holds - re-evaluation is an update, never a duplicate. The name
// lexes through the base statement table, giving it the same collision rules the source has (a
// mnemonic, keyword or built-in constant is refused), and the expression evaluates exactly like an
// assignment's, so it may forward-reference symbols the source defines later. Anything left over
// after the expression is a mistake: there is no next statement to hand it to.
static parse_result apply_define(baron *b, rc_str define, parse_flags flags, rc_arena scratch)
{
    rc_mstr name = rc_mstr_make(define.len + 4, &scratch);
    rc_mstr_append(&name, RC_STR("-D "), &scratch);
    rc_mstr_append(&name, define, &scratch);
    uint32_t src = source_files_add_string(&b->source_files, name.view, define);

    rc_str text = source_files_text(&b->source_files, src);
    cursor def  = {.source = src, .pos = 0};

    lexer_result nm = lexer_next(text, 0, base_statement_tokens);
    if (nm.token.type != lexeme_type_identifier) {
        return syntax_error(b, error_type_expected_var_name, def);
    }
    if (is_dotted(nm.token.identifier.name)) {
        return syntax_error(b, error_type_invalid_assignment, def);
    }

    lexer_result eq = lexer_next(text, nm.next, assign_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return syntax_error(b, error_type_expected_assign, (cursor) {.source = src, .pos = nm.next});
    }

    expr_result e = eval(b, (cursor) {.source = src, .pos = eq.next}, 0, sections_default, scratch);
    if (e.error != expr_error_none) {
        return syntax_error(b, error_type_expression, (cursor) {.source = src, .pos = e.error_at});
    }
    if (!lexer_at_end(text, lexer_skip_whitespace(text, e.next))) {
        return syntax_error(b, error_type_expected_end_of_expression, (cursor) {.source = src, .pos = e.next});
    }

    // Bind it, mirroring handle_assignment: a duplicate here is a second -D of the same name (a
    // source assignment to it collides at ITS site instead - we bound first), a forward reference
    // defers to the next pass, and a moved value is the convergence signal.
    parse_result r = {.next = e.next};
    symbol_status st = scopes_set_symbol(&b->scopes, 0, nm.token.identifier.name, e.value, def);

    if (st == symbol_status_duplicate) {
        semantic_error_payload(b, flags, error_type_duplicate_symbol, def, nm.token.identifier.name);
        cursor original = scopes_symbol_def(&b->scopes, 0, nm.token.identifier.name);
        if (!cursor_is_none(original)) {
            semantic_error_payload(b, flags, error_type_original_definition, original, nm.token.identifier.name);
        }
    }
    else {
        if (value_is_error(e.value)) {
            if (e.value.error.code == error_type_unknown_symbol) {
                semantic_error_payload(b, flags, error_type_undefined_symbol,
                                       (cursor) {.source = src, .pos = eq.next}, e.value.error.detail);
                if (!flags.final) r.unresolved = true;   // a forward reference into the source; settles on a later pass
            }
            else {
                semantic_error_payload(b, flags, e.value.error.code,
                                       (cursor) {.source = src, .pos = eq.next}, e.value.error.detail);
            }
        }
        r.changed = (st == symbol_status_changed);
    }

    // Echo it in the listing like the assignment it is.
    verbose_text_line(b, flags, def, text.len, 0, verbose_text_margin);

    return r;
}

// All the -D definitions, in command-line order, folded into one result for the pass to carry. A
// malformed definition is fatal - it can never come right on a later pass - so we stop at the first.
static parse_result apply_defines(baron *b, parse_flags flags, rc_arena scratch)
{
    parse_result r = {0};
    for (uint32_t i = 0; i < b->defines.num && !r.fatal; i++) {
        r = fold(r, apply_define(b, rc_view_str_get(b->defines, i), flags, scratch));
    }
    return r;
}

static parse_result run_pass(baron *b, uint32_t source, parse_flags flags, rc_arena scratch)
{
    // The per-pass arena backs sections, macros and functions - a fresh projection of the source each pass.
    // Reset it once, then rebuild all three stores into it (sections gets a fresh default section; the two
    // token tables are reseeded from their static bases, with room for per-name tokens).
    rc_arena_reset(b->per_pass);
    sections_reset(&b->sections);
    if (!flags.output) {
        // ZPRESERVE re-runs this pass and refills the (permanent) set. The OUTPUT pass leaves the whole
        // zeropage subsystem alone: allocation already ran, and the final pass's IR/vars stay readable
        // (everything that appends to them is final-gated; ZPRESERVE re-marking its bits is idempotent).
        zeropage_reset(&b->zeropage);
    }
    b->include_depth   = 0;                  // balanced by handle_include, but a fatal unwind skips the decrement
    b->macro_depth     = 0;                  // ditto for macro expansion
    b->function_depth  = 0;                  // ditto for FUNCTION recursion (balanced by the evaluator)
    expression_reset_random();               // replay the same RND stream every pass, so RND can converge

    macros_reset(&b->macros, base_statement_tokens, 128);
    functions_reset(&b->functions, expression_operand_base(), 128);

    // Fresh channel buffers each pass (the previous pass's died with the arena reset above). They fill
    // lazily on first append - only the output pass writes them at all - except channel 0 on the listing
    // pass, which is certain to grow and gets its buffer up front.
    for (uint32_t i = 0; i < baron_num_channels; i++) {
        b->channels[i] = (rc_mstr) {0};
    }
    if (flags.listing) {
        b->channels[0] = rc_mstr_make(4096, b->per_pass);
    }

    const uint32_t scope   = 0;
    const uint32_t section = sections_default;   // each pass starts in the default section (index 0)

    // The command-line predefines bind first, so the very first statement can already read them; their
    // forward references fold into the pass result and settle with everything else.
    parse_result r = apply_defines(b, flags, scratch);
    if (r.fatal) {
        return r;
    }
    r = fold(r, parse_file(
        b,
        (cursor) {.source = source, .pos = 0},
        scope,
        section,
        flags,
        scratch
    ));

    // INCSECTION bookkeeping. A reservation that missed its source's settled size reshapes the layout,
    // so it forces another pass; a dependency cycle can never settle, so it fails RIGHT NOW (on the first
    // pass that sees it, not after the pass cap); and every named section's size rolls into the
    // cross-pass map, ready for next pass's reservations.
    if (!r.fatal) {
        r.changed |= sections_splices_changed(&b->sections);
        if (!splices_resolve(b, false, scratch)) {
            r.fatal = true;
        }
        sections_note_sizes(&b->sections);
        // Convergence hardening: emission that shifted with no symbol moving still owes another pass
        // (an RND draw set displaced by a settling structure, say). Settling passes only - the final
        // pass differs legitimately at INCBIN spans, the output pass at every ZPAUTO address.
        if (!flags.final && !flags.output) {
            r.changed |= sections_emission_changed(&b->sections);
        }
    }
    return r;
}

// Does this call definitely rewrite variable `v` whichever arm it takes? True only when every arm is an
// in-program routine whose must-write set covers the whole variable - an external arm returns having
// written nothing, and an untrackable one could do anything, so both forfeit the kill.
static bool call_rewrites(const liveness *lv, call_targets ct, uint32_t v)
{
    if (ct.unknown || ct.external || ct.blocks.view.num == 0) {
        return false;
    }
    for (uint32_t c = 0; c < ct.blocks.view.num; c++) {
        if (!rc_bitset_is_set(&lv->must_write[rc_array_u32_get(&ct.blocks, c)], v)) {
            return false;
        }
    }
    return true;
}

// Mark into `reach` every block reachable from `seed`, following the same edges control can take: the CFG's
// successor slices (fall-throughs, branches, wired CANJUMP arms) plus each call's resolved callee entries.
// An unknown or external call arm contributes nothing - external code is off the map, and an unannotated
// computed call cannot extend reachability (its true callees may then warn, which is exactly the "add
// CANCALL" nudge). `within`, when non-NULL, restricts the walk to blocks inside that set - how the region
// closures below stay within the unreachable half of the graph.
static void reach_from(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, uint32_t seed,
                       const rc_bitset *within, rc_bitset *reach, rc_arena scratch)
{
    if (seed >= g.blocks.num || rc_bitset_is_set(reach, seed)
        || (within != NULL && !rc_bitset_is_set(within, seed))) {
        return;
    }
    uint32_t *stack = rc_arena_alloc_type(&scratch, uint32_t, g.blocks.num);
    uint32_t  sp    = 0;
    stack[sp++] = seed;
    rc_bitset_set(reach, seed);
    while (sp > 0) {
        basic_block blk = rc_array_basic_block_get(&g.blocks, stack[--sp]);
        for (uint32_t i = 0; i < blk.num_insns; i++) {
            zp_insn n = rc_view_zp_insn_get(insns, blk.first_insn + i);
            if (n.flow != zp_flow_call) {
                continue;
            }
            call_targets ct = cfg_call_targets(g, cflows, n, &scratch);
            for (uint32_t c = 0; c < ct.blocks.view.num; c++) {
                uint32_t t = rc_array_u32_get(&ct.blocks, c);
                if (!rc_bitset_is_set(reach, t) && (within == NULL || rc_bitset_is_set(within, t))) {
                    rc_bitset_set(reach, t);
                    stack[sp++] = t;
                }
            }
        }
        for (uint32_t k = 0; k < blk.succ_count; k++) {
            uint32_t t = cfg_succ(g, blk, k);
            if (!rc_bitset_is_set(reach, t) && (within == NULL || rc_bitset_is_set(within, t))) {
                rc_bitset_set(reach, t);
                stack[sp++] = t;
            }
        }
    }
}

// Pin variable `v` against every other variable: both interference directions, the whole registry. The
// hammer for state an interrupt handler shares with the rest of the program - no byte reuse, ever.
static void pin_var(liveness *lv, uint32_t v, uint32_t nv)
{
    for (uint32_t w = 0; w < nv; w++) {
        if (w != v) {
            rc_bitset_set(&lv->interfere[v], w);
            rc_bitset_set(&lv->interfere[w], v);
        }
    }
}

// Post-convergence zero-page allocation. Layout has settled with every ZPAUTO reference sized as a
// placeholder zero-page access, so assigning a real byte and patching the operand cannot perturb size. The
// governing rule is CERTAINTY: this only patches a program it can prove correct, and turns anything it cannot
// into a clear diagnostic pointing the user at a fix or an annotation. Three refusals:
//   - a computed / indirect jump reaches code the CFG cannot follow (unknown_succ) -> error_type_zpauto_computed_flow;
//   - a variable is live across a JSR whose callee footprint cannot be bounded (the callee reaches computed
//     flow) -> error_type_zpauto_across_call;
//   - a variable the recursion FRESHLY writes is held live across the recursive call (a per-level value one
//     static byte cannot serve) -> error_type_zpauto_recursion; a read/accumulated value across recursion is fine;
//   - more simultaneously-live variables than reserved bytes -> error_type_zeropage_full (a spill).
// On any refusal it records the error(s) and patches nothing; run_passes then fails the assemble. Only a
// fully analysable, colourable program has its operands + symbols rewritten to real addresses.
// One deliberate leniency: a transfer to a CONSTANT destination matching nothing we assembled - JSR &FFEE,
// JMP &FFEE, or JMP (&FFFC) through a fixed OS vector - is a transfer OUT of the program. External code
// touches none of our variables (ZPRESERVE names exactly the bytes nothing outside the program uses), so
// such a call has an empty footprint and such a jump is a clean exit; neither needs an annotation.
// `work` (cfg + liveness results) and `scratch` (their by-value scratch) are two working arenas the caller
// hands in BY VALUE; nothing here outlives the call, so both are reclaimed by the caller. They must have
// distinct backing (the scratch-aliasing lesson), which the caller guarantees by passing two different arenas.
static void zeropage_finalize(baron *b, rc_arena work, rc_arena scratch)
{
    if (!zeropage_is_enabled(&b->zeropage)) {
        return;
    }
    zeropage_resolve_vregs(&b->zeropage);   // map each operand's def cursor to its vreg (registry now complete)
    rc_view_zp_insn insns = zeropage_insns(&b->zeropage);
    uint32_t nv = zeropage_var_count(&b->zeropage);

    bool refused = false;

    // A note on layout soundness: the CFG identifies a block by (section, pc), so two sections sharing an
    // address (paged banks) are distinct - fall-through and in-section branches stay within a section, and a
    // control transfer that crosses one does so by naming a label, which picks the section. The one thing that
    // would break WITHIN a section - two instructions at one address - cannot happen: a section's org is fixed
    // at open and its cursor only advances (emission, or a forward SKIP/SKIPTO/ALIGN), so its pc is strictly
    // monotonic. cfg_build asserts that invariant in debug builds; there is nothing to refuse here.

    // Guard 0 (indexed access is the user's responsibility): a variable reached by an indexed / indexed-
    // indirect mode (var,X, var,Y, (var,X)) touches var+index at run time - the intended way to walk a ZPAUTO
    // table. The allocator reserves the WHOLE variable [base, base+width) and liveness marks it all live on any
    // access, so an index that stays inside the width only ever reads the variable's own bytes: sound. What
    // Baron cannot see is the run-time index - an index at or past the width walks into a neighbour, and no
    // static check can catch that. So this is not a refusal but an OPT-IN warning (severity_optional, silent
    // until the warning level is raised), the same trust we place in CANCALL / UNREACHABLE. The CONSTANT base
    // of the access is still bounds-checked below (Guard 0b): LDA var+4,X on a 4-wide table is refused because
    // the base is already off the end before any index is added. (Checked per recorded insn once vregs are
    // resolved: a var_indexed insn that really names a ZPAUTO now has a vreg.)
    // Guard 0b (bounds): a var+offset access must lie WITHIN the variable's declared width. The access spans
    // `access` bytes at `var_offset`: 1 for a direct byte, 2 for an indirect pointer deref ((var),Y / (var),
    // which reads the pointer's low+high bytes). If [offset, offset+access) runs off the end, the stray byte is
    // one the allocator never reserved for this variable - so refuse. A 1-byte ZPAUTO1 used as a pointer is the
    // special case that gets the pointed "declare it ZPAUTO2" message; every other overrun (a var+n past a
    // table's end, a pointer straddling the top of a wider var) gets the general out-of-bounds message. (Both
    // need the resolved vreg for the width, hence here rather than at record time; an unknown offset is skipped.)
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.vreg == RC_INDEX_NONE) {
            continue;
        }
        if (n.var_indexed) {
            baron_warning_payload(b, error_type_zpauto_indexed_access, n.at, severity_optional,
                                  zeropage_var_get(&b->zeropage, n.vreg).name);
        }
        if (n.var_offset != RC_INDEX_NONE) {
            uint32_t width  = zeropage_var_get(&b->zeropage, n.vreg).width;
            uint32_t access = n.var_indirect ? 2u : 1u;
            if ((uint64_t) n.var_offset + access > width) {
                error_type code = (n.var_indirect && width < 2)
                                      ? error_type_zpauto_narrow_pointer   // never wide enough to be a pointer
                                      : error_type_zpauto_out_of_bounds;
                baron_error(b, code, n.at);
                refused = true;
            }
        }
    }

    // A broken layout or an out-of-envelope access is decided from the instruction stream alone - and building
    // a CFG over a stream we have just judged unanalysable would be meaningless. Bail before we do.
    if (refused) {
        return;
    }

    rc_view_zp_cflow cflows = zeropage_cflows(&b->zeropage);

    // cfg_build applies the control-flow annotations itself: an UNREACHABLE prunes a branch's dead fall-through
    // edge, and a CANJUMP wires a computed JMP's declared targets (clearing the taint that would refuse it).
    cfg g       = cfg_build(insns, cflows, zeropage_labels(&b->zeropage), zeropage_entries(&b->zeropage),
                            &work, scratch);
    // No entry block: the old "block 0 is the program entry" presumption is retired (roots are handled
    // below), so the synthetic pairwise-input rule and the input classification stay dormant here. A var
    // read before any write from an entry has no defined value, and gets no protection - by design.
    liveness lv = liveness_analyze(g, insns, cflows, zeropage_vars(&b->zeropage), RC_INDEX_NONE,
                                   &work, scratch);

    // A variable no instruction touches gets a WARNING, no address, and no definition (the rewrite below
    // removes its binding): a declaration costing a byte of the pool for nothing is more likely a leftover
    // than intentional. One warning per declaration site - a macro / FOR body's instantiations share one
    // def cursor, so an unused declaration inside a 2000-iteration loop speaks once, not 2000 times.
    for (uint32_t v = 0; v < nv; v++) {
        if (liveness_class_of(&lv, v) != vreg_class_unused) {
            continue;
        }
        zp_var var = zeropage_var_get(&b->zeropage, v);
        bool already = false;
        for (uint32_t u = 0; u < v && !already; u++) {
            already = liveness_class_of(&lv, u) == vreg_class_unused
                   && cursor_is_equal(zeropage_var_get(&b->zeropage, u).def, var.def);
        }
        if (!already) {
            baron_warning_payload(b, error_type_zpauto_unused, var.def, severity_warning, var.name);
        }
    }

    // The root set: where control can enter the program from outside. Declared ZPENTRY / ZPINTERRUPT
    // markers resolve to their blocks (the cfg marked each a leader, so "no block" reliably means the
    // marker sits on no instruction - data, or a section's end - a static mistake we refuse). Any ZPENTRY
    // replaces the default sync roots; without one, each section's first recorded block roots itself - the
    // generalisation of the old "block 0 is the entry" presumption, kind to the relocation workflow where
    // a spliced section's code is entered at its own org. ZPINTERRUPT alone leaves the defaults in place
    // (a handler is extra, not a statement about where the mainline starts).
    rc_view_zp_entry entries = zeropage_entries(&b->zeropage);
    uint32_t  nb           = g.blocks.num;
    rc_bitset roots        = {0};
    rc_bitset handler_seen = {0};
    rc_bitset_resize(&roots, nb ? nb : 1, &scratch);
    rc_bitset_resize(&handler_seen, nb ? nb : 1, &scratch);
    uint32_t *handler_blocks = entries.num ? rc_arena_alloc_type(&scratch, uint32_t, entries.num) : NULL;
    cursor   *handler_ats    = entries.num ? rc_arena_alloc_type(&scratch, cursor, entries.num) : NULL;
    uint32_t  num_handlers   = 0;
    bool      any_sync       = false;
    bool      entry_unknown  = false;
    for (uint32_t i = 0; i < entries.num; i++) {
        zp_entry e = rc_view_zp_entry_get(entries, i);
        if (!e.interrupt) {
            any_sync = true;   // the declaration replaces the defaults even if it fails to resolve
        }
        uint32_t bi = cfg_block_at(g, e.section, e.pc);
        if (bi == RC_INDEX_NONE) {
            baron_error(b, error_type_zpentry_no_code, e.at);
            refused = entry_unknown = true;
            continue;
        }
        rc_bitset_set(&roots, bi);   // duplicates, and ZPENTRY + ZPINTERRUPT on one pc, collapse here
        if (e.interrupt && !rc_bitset_is_set(&handler_seen, bi)) {
            rc_bitset_set(&handler_seen, bi);
            handler_blocks[num_handlers] = bi;
            handler_ats[num_handlers]    = e.at;
            num_handlers++;
        }
    }
    if (!any_sync) {
        // Blocks are recorded in stream order and a section's pc only advances, so the first block seen
        // carrying each section index is that section's earliest code. A data-only section has no blocks,
        // hence no root - correctly nothing to reach.
        uint32_t max_section = 0;
        for (uint32_t bi = 0; bi < nb; bi++) {
            uint32_t sec = rc_array_basic_block_get(&g.blocks, bi).section;
            if (sec > max_section) {
                max_section = sec;
            }
        }
        rc_bitset section_seen = {0};   // indexed by section, and data-only sections can leave gaps
        rc_bitset_resize(&section_seen, max_section + 1, &scratch);
        for (uint32_t bi = 0; bi < nb; bi++) {
            uint32_t sec = rc_array_basic_block_get(&g.blocks, bi).section;
            if (!rc_bitset_is_set(&section_seen, sec)) {
                rc_bitset_set(&section_seen, sec);
                rc_bitset_set(&roots, bi);
            }
        }
    }

    // The reachability warning: code touching a ZPAUTO variable that no root can reach is either an
    // undeclared handler (the silently-unsound shape this whole feature exists to catch) or dead code.
    // One warning per REGION, at its head (the natural "put your marker here" spot), not per block.
    // Skipped when a marker failed to resolve - an under-approximate root set would spray false alarms,
    // and the error above already fails the assemble.
    if (!entry_unknown && nv > 0 && nb > 0) {
        rc_bitset reach = {0};
        rc_bitset_resize(&reach, nb, &scratch);
        for (uint32_t r = rc_bitset_get_first_set(&roots); r != RC_INDEX_NONE;
             r = rc_bitset_get_next_set(&roots, r + 1)) {
            reach_from(g, insns, cflows, r, NULL, &reach, scratch);
        }
        rc_bitset offending = {0};
        rc_bitset_resize(&offending, nb, &scratch);
        bool any_offending = false;
        for (uint32_t bi = 0; bi < nb; bi++) {
            basic_block blk = rc_array_basic_block_get(&g.blocks, bi);
            for (uint32_t i = 0; !rc_bitset_is_set(&reach, bi) && i < blk.num_insns; i++) {
                zp_insn n = rc_view_zp_insn_get(insns, blk.first_insn + i);
                if (n.vreg != RC_INDEX_NONE && !n.var_kill) {   // a stray DISCARD is not a use worth warning over
                    rc_bitset_set(&offending, bi);
                    any_offending = true;
                    break;
                }
            }
        }
        if (any_offending) {
            // A region head is an unreachable block with no unreachable predecessor - over the same edge
            // set the walk follows. Warn once per head whose closure holds an uncovered offender; a pure
            // cycle has no head, so a mop-up sweep catches anything the heads did not claim.
            rc_bitset unreached = {0};
            rc_bitset_resize(&unreached, nb, &scratch);
            bool *upred = rc_arena_alloc_zero_type(&scratch, bool, nb);
            for (uint32_t bi = 0; bi < nb; bi++) {
                if (!rc_bitset_is_set(&reach, bi)) {
                    rc_bitset_set(&unreached, bi);
                }
            }
            for (uint32_t bi = 0; bi < nb; bi++) {
                if (rc_bitset_is_set(&reach, bi)) {
                    continue;
                }
                basic_block blk = rc_array_basic_block_get(&g.blocks, bi);
                for (uint32_t i = 0; i < blk.num_insns; i++) {
                    zp_insn n = rc_view_zp_insn_get(insns, blk.first_insn + i);
                    if (n.flow != zp_flow_call) {
                        continue;
                    }
                    call_targets ct = cfg_call_targets(g, cflows, n, &scratch);
                    for (uint32_t c = 0; c < ct.blocks.view.num; c++) {
                        uint32_t t = rc_array_u32_get(&ct.blocks, c);
                        if (!rc_bitset_is_set(&reach, t)) { upred[t] = true; }
                    }
                }
                for (uint32_t k = 0; k < blk.succ_count; k++) {
                    uint32_t t = cfg_succ(g, blk, k);
                    if (!rc_bitset_is_set(&reach, t)) { upred[t] = true; }
                }
            }
            rc_bitset covered = {0};
            rc_bitset_resize(&covered, nb, &scratch);
            for (uint32_t pass = 0; pass < 2; pass++) {
                for (uint32_t bi = 0; bi < nb; bi++) {
                    bool head = pass == 0 ? (!rc_bitset_is_set(&reach, bi) && !upred[bi])
                                          : (rc_bitset_is_set(&offending, bi) && !rc_bitset_is_set(&covered, bi));
                    if (!head) {
                        continue;
                    }
                    rc_bitset closure = {0};
                    rc_bitset_resize(&closure, nb, &scratch);
                    reach_from(g, insns, cflows, bi, &unreached, &closure, scratch);
                    bool fresh = false;
                    for (uint32_t c = rc_bitset_get_first_set(&closure); !fresh && c != RC_INDEX_NONE;
                         c = rc_bitset_get_next_set(&closure, c + 1)) {
                        fresh = rc_bitset_is_set(&offending, c) && !rc_bitset_is_set(&covered, c);
                    }
                    if (fresh) {
                        basic_block blk = rc_array_basic_block_get(&g.blocks, bi);
                        baron_warning(b, error_type_zpauto_unreachable,
                                      rc_view_zp_insn_get(insns, blk.first_insn).at, severity_warning);
                    }
                    rc_bitset_union(&covered, &closure);
                }
            }
        }
    }

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
    rc_bitset_resize(&live, nv, &scratch);
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
            if (n.flow == zp_flow_call) {
                call_targets ct = cfg_call_targets(g, cflows, n, &scratch);
                if (rc_bitset_get_first_set(&live) != RC_INDEX_NONE) {
                    // What this call reaches - CANCALL overrides an untrackable literal target with a declared set.
                    footprint fp = footprint_of_call(g, insns, cflows, n, nv, &work, scratch);
                    if (fp.unknown_call) {
                        baron_error(b, error_type_zpauto_across_call, n.at);
                        refused = true;
                    }
                    else if (fp.recursive && rc_bitset_intersects(&live, &fp.killed)) {
                        // Recursion is fatal only for a value the cycle FRESHLY writes (a write-only def) and
                        // carries live across itself: each level would want its own byte. A value merely read or
                        // accumulated (DEC/INC) across the recursion shares one byte safely, so it falls through
                        // to interference. Judged on the UNREDUCED live set, deliberately: reading a value back
                        // after a recursive call that rewrites it IS the per-level pattern, however definite the
                        // cycle's write - the must-write kill below must not hide it.
                        baron_error(b, error_type_zpauto_recursion, n.at);
                        refused = true;
                    }
                    else {
                        // Every var live across this call interferes with every vreg the callee touches - except
                        // one the call definitely REWRITES: its post-call value is born inside the callee (where
                        // ordinary liveness already fences it), not carried across, so fencing it off the
                        // callee's whole workspace would forbid exactly the reuse must-write proves safe.
                        for (uint32_t c = rc_bitset_get_first_set(&live); c != RC_INDEX_NONE;
                             c = rc_bitset_get_next_set(&live, c + 1)) {
                            if (call_rewrites(&lv, ct, c)) {
                                continue;
                            }
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
                // Step backward over the call, mirroring the dataflow's transfer at this sweep's variable
                // granularity: the callees' definite writes end the pre-call values, and their inputs are the
                // arguments the call consumes - live from here back to their stores, so an argument counts as
                // live across any EARLIER call it must survive (but never across its own).
                for (uint32_t v = 0; v < nv; v++) {
                    if (call_rewrites(&lv, ct, v)) {
                        rc_bitset_clear(&live, v);
                    }
                }
                for (uint32_t c = 0; c < ct.blocks.view.num; c++) {
                    uint32_t e = rc_array_u32_get(&ct.blocks, c);
                    for (uint32_t v = 0; v < nv; v++) {
                        if (liveness_is_live_in(&lv, e, v)) {
                            rc_bitset_set(&live, v);
                        }
                    }
                }
            }
            if (n.vreg != RC_INDEX_NONE) {
                // A write ends the variable's range only when it covers the whole variable on its own
                // (zp_insn_write_kills): a partial write - the LSB store of a pointer - preserves the
                // bytes it does not touch, so the old value stays live through it. This sweep runs at
                // variable granularity, so a store PAIR that fully rewrites a pointer conservatively
                // keeps it live too - a too-live set only adds interference edges, never misses any.
                if (n.var_kill) {
                    rc_bitset_clear(&live, n.vreg);   // a DISCARD ends the whole variable's range, pinning nothing
                }
                else if (zp_insn_write_kills(n, zeropage_var_get(&b->zeropage, n.vreg).width)) {
                    rc_bitset_clear(&live, n.vreg);
                }
                else if (n.rw & vref_write) {
                    rc_bitset_set(&live, n.vreg);
                }
                if (n.rw & vref_read) { rc_bitset_set(&live, n.vreg); }
            }
        }
    }

    // Guard 3 / interrupt pinning: a ZPINTERRUPT handler preempts at ARBITRARY instructions, so no
    // transaction discipline can be assumed around it. Two rules make its variables sound: (i) its
    // communication vars - live-in at the handler's entry, written by the mainline for the handler to read -
    // are pinned against everything, the handler's own temps included (the mainline may rewrite one at any
    // moment relative to the handler's execution, so no instant of "dead" exists to reuse); (ii) every var
    // in the handler's transitive footprint interferes with every var outside it - a handler temp can never
    // share a byte with mainline state it might fire on top of. Among the handler's own temps, ordinary
    // liveness still governs, so intra-handler reuse survives. A footprint the walk cannot bound (an
    // unannotated computed call in the extent) is refused, with the same remedy as ever: CANCALL.
    for (uint32_t h = 0; h < num_handlers; h++) {
        footprint fp = footprint_compute(g, insns, cflows, handler_blocks[h], nv, &work, scratch);
        if (fp.unknown_call) {
            baron_error(b, error_type_zpauto_across_call, handler_ats[h]);
            refused = true;
            continue;
        }
        for (uint32_t v = 0; v < nv; v++) {
            if (liveness_is_live_in(&lv, handler_blocks[h], v)) {
                pin_var(&lv, v, nv);
            }
        }
        for (uint32_t t = rc_bitset_get_first_set(&fp.touched); t != RC_INDEX_NONE;
             t = rc_bitset_get_next_set(&fp.touched, t + 1)) {
            for (uint32_t v = 0; v < nv; v++) {
                if (v != t && !rc_bitset_is_set(&fp.touched, v)) {
                    rc_bitset_set(&lv.interfere[t], v);
                    rc_bitset_set(&lv.interfere[v], t);
                }
            }
        }
    }

    // The ZPENTRY input warning: an externally-called routine that reads a variable before writing it
    // expects its caller to have poked the value - but an outside caller cannot know an allocator-chosen
    // address, so a ZPAUTO input on a declared external interface is almost certainly a mistake (an
    // external interface wants fixed bytes: ZPRESERVE them, or use plain addresses). The test is the
    // read-before-write walk, not live-in: the backward fixpoint's shared return edges smear one call
    // site's live-after through a common helper into another call site (a helper called both before the
    // entry's init and from the main loop makes every loop-carried variable look live-in at the entry),
    // while the walk follows calls with per-callee summaries, so only genuine uninitialised reads count.
    // Handler blocks are excluded - a stacked ZPENTRY+ZPINTERRUPT takes the stricter interrupt treatment,
    // and Guard 3's pinned comm vars ARE the supported live-in pattern there.
    rc_bitset sync_seen = {0};
    rc_bitset_resize(&sync_seen, nb ? nb : 1, &scratch);
    for (uint32_t i = 0; nv > 0 && i < entries.num; i++) {
        zp_entry e = rc_view_zp_entry_get(entries, i);
        if (e.interrupt) {
            continue;
        }
        uint32_t bi = cfg_block_at(g, e.section, e.pc);
        if (bi == RC_INDEX_NONE || rc_bitset_is_set(&handler_seen, bi) || rc_bitset_is_set(&sync_seen, bi)) {
            continue;   // unresolved already errored above; duplicates collapse to one report
        }
        rc_bitset_set(&sync_seen, bi);
        rc_bitset inputs = liveness_read_before_write(g, insns, cflows, zeropage_vars(&b->zeropage),
                                                      bi, &work, scratch);
        for (uint32_t v = rc_bitset_get_first_set(&inputs); v != RC_INDEX_NONE;
             v = rc_bitset_get_next_set(&inputs, v + 1)) {
            baron_warning_payload(b, error_type_zpentry_input, e.at, severity_warning,
                                  zeropage_var_get(&b->zeropage, v).name);
        }
    }

    if (!refused) {
        zp_coloring col = zp_color(&lv, zeropage_vars(&b->zeropage), zeropage_reserved(&b->zeropage),
                                   &work, scratch);
        if (col.any_spilled) {
            // An unplaced USED variable is a spill; an unused one was deliberately skipped (warned above).
            for (uint32_t v = 0; v < nv; v++) {
                if (col.base[v] == RC_INDEX_NONE && liveness_class_of(&lv, v) != vreg_class_unused) {
                    zp_var var = zeropage_var_get(&b->zeropage, v);
                    baron_error_payload(b, error_type_zeropage_full, var.def, var.name);
                }
            }
        }
        else {
            // Rewrite each variable's symbol from the typed placeholder to its real zero-page address;
            // the OUTPUT pass then re-emits every operand and data byte against the real values (there
            // is no operand patching - a settling pass's bytes hold only intra-variable offsets). An
            // unused variable's binding is REMOVED instead: it has no address to give. Gone from the
            // symbol table (and so from the -v listing's `var = &xx [auto]` lines), it is exactly as
            // if the declaration were not there.
            for (uint32_t v = 0; v < nv; v++) {
                zp_var var = zeropage_var_get(&b->zeropage, v);
                if (col.base[v] == RC_INDEX_NONE) {
                    scopes_remove_symbol(&b->scopes, var.scope, var.name);
                }
                else {
                    scopes_set_symbol(&b->scopes, var.scope, var.name,
                                      value_make_numeric((double) col.base[v]), var.def);
                }
            }
        }
    }
}

// Discard the half-built outputs - object code, symbols and the listing - so a failed assemble hands
// back nothing to consume; only the diagnostics remain. Returns 0, the failure signal the entry points
// hand back.
static uint32_t assemble_failed(baron *b)
{
    sections_reset(&b->sections);   // a fresh, empty default section - so a failed read hands back no code
    scopes_reset(&b->scopes);
    for (uint32_t i = 0; i < baron_num_channels; i++) {
        b->channels[i] = (rc_mstr) {0};   // no PRINT output / listing from a failed assemble
    }
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
            // Layout has settled: now assign real zero-page bytes to the ZPAUTO variables and rewrite
            // their symbols to the chosen addresses. This refuses (records errors, assigns nothing) on
            // anything it cannot prove correct, so a fresh error here fails the assemble just like a
            // pass error would.
            zeropage_finalize(b, *b->per_pass, scratch);
            if (baron_has_errors(b)) {
                return assemble_failed(b);
            }
            // The OUTPUT pass: one re-emission with the ZPAUTO symbols holding their allocated
            // addresses, whose sections ARE the result (there is no operand patching - a settling
            // pass's operands hold intra-variable offsets, meaningless as output). It must run
            // whenever the zp feature is on; -v rides the same pass to build the listing text, with
            // the true bytes in every line. Re-emission cannot move anything: a ZPAUTO address is a
            // typed value that every layout-affecting context refuses, and the contexts that accept
            // one (instruction operands, data elements) are width-stable, so the converged layout is
            // reproduced exactly. It runs final=false, so nothing gated on the settling pass
            // (diagnostics, the zeropage IR) records twice.
            if (b->want_verbose || zeropage_is_enabled(&b->zeropage)) {
                parse_result out = run_pass(b, source,
                    (parse_flags) {.active = true, .output = true, .listing = b->want_verbose}, scratch);
                if (out.fatal || baron_has_errors(b)) {
                    return assemble_failed(b);
                }
            }
            // The INCSECTION fixup, absolutely last: after the output pass (so a spliced copy carries
            // the allocated addresses, and the rebuilt sections are the ones the result snapshots).
            // splices_resolve copies in dependency order; an unknown source is judged - and refused -
            // only here, once every IF arm has settled.
            if (!splices_resolve(b, true, scratch)) {
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
// object code) lives in the per_pass arena from the final pass; diagnostics, the source cache and the scope
// tree's backing (nodes + trie pools) are in permanent. All are handed back as cheap views/projections that
// outlive `b` (which dies the moment we return) - nothing is flattened up front; the caller queries on demand.
static baron_result baron_result_make(baron *b, uint32_t passes)
{
    baron_result r = {
        .passes      = passes,
        .sections    = sections_all(&b->sections),
        .diagnostics = b->diagnostics.view,
        .sources     = b->source_files.nodes.view,
        .scopes      = scopes_view_make(&b->scopes),
    };
    for (uint32_t i = 0; i < baron_num_channels; i++) {
        r.channels[i] = b->channels[i].view;
    }
    return r;
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

baron_result assemble_string(baron_desc *desc, rc_str name, rc_str text)
{
    RC_ASSERT(desc != NULL);
    baron b = baron_make(desc);
    uint32_t source = source_files_add_string(&b.source_files, name, text);
    return baron_result_make(&b, run_passes(&b, source, desc->scratch));
}

baron_result assemble_file(baron_desc *desc, rc_str path)
{
    RC_ASSERT(desc != NULL);
    baron b = baron_make(desc);
    uint32_t source = source_files_add_file(&b.source_files, path);
    if (source == RC_INDEX_NONE) {
        baron_error(&b, error_type_source_load, (cursor) {0});
        return baron_result_make(&b, assemble_failed(&b));   // assemble_failed clears outputs and returns 0
    }
    return baron_result_make(&b, run_passes(&b, source, desc->scratch));
}




#ifdef BARON_TESTS

#include "richc/test.h"
#include "richc/mstr.h"   // the stress test builds a big source with rc_mstr

RC_TEST_GROUP_DATA(assemble) {
    baron_desc desc;
    baron_result r;      // the last assemble's snapshot; the helpers below read it back
};

RC_TEST_GROUP_INIT(assemble, fix)
{
    fix->desc = (baron_desc) {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };
}

RC_TEST_GROUP_DEINIT(assemble, fix)
{
    rc_arena_deinit(&fix->desc.permanent);
    rc_arena_deinit(&fix->desc.per_pass);
    rc_arena_deinit(&fix->desc.scratch);
}

// Assemble a snippet, stash its result in fix->r, and yield the pass count (0 on failure). Each call is an
// independent assemble on the shared desc, so distinct snippets need no distinct names. Object code,
// symbols and diagnostics are then read back from fix->r via the helpers.
#define ASM(src) (fix->r = assemble_string(&fix->desc, RC_STR(src), RC_STR(src)), fix->r.passes)

// The verbose listing of the last ASM - channel 0, built on the listing pass, so it shows the final
// (post-allocation) bytes with any PRINT output interleaved. Tests compare it whole: a mis-set column
// or a stray line fails loudly and prints the actual text.
#define VERB() (fix->r.channels[0])

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

// How many diagnostics carry `code` - for the once-per-region promises, where "fired" is not enough.
static uint32_t diag_count(const baron_result *r, error_type code)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->diagnostics.num; i++) {
        if (rc_view_diagnostic_get(r->diagnostics, i).code == code) {
            n++;
        }
    }
    return n;
}

// The payload of the first diagnostic carrying `code` ({0} when none does).
static rc_str diag_payload(const baron_result *r, error_type code)
{
    for (uint32_t i = 0; i < r->diagnostics.num; i++) {
        diagnostic d = rc_view_diagnostic_get(r->diagnostics, i);
        if (d.code == code) {
            return d.payload;
        }
    }
    return (rc_str) {0};
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

RC_TEST_STEP(assemble, section_cursor_is_independent, fix)
{
    // org is an INHERITED attribute, not a running cursor. A sibling section with no org of its own inherits
    // the default section's org (0) - it does NOT continue from where the previous section ended. `a` runs at
    // &2000 (x at &2000); `b` has no org, so it inherits 0 and y binds at 0, not &2001.
    RC_CHECK_TRUE(ASM("SECTION a, org=&2000 : .x EQUB 0 : ENDSECTION : SECTION b : .y EQUB 0 : ENDSECTION") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("x")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("y")), value_make_numeric(0x0000)));

    // A NESTED section with no org starts at its PARENT's org (base address), not wherever the parent has
    // emitted to - and the parent's cursor is untouched by the child. `outer` runs at &3000, emits a byte
    // (o at &3000, cursor now &3001); nested `inner` inherits outer's org &3000, so p binds at &3000
    // (overlapping outer - they are separate spaces); back in outer, q binds at &3001, not past inner.
    RC_CHECK_TRUE(ASM("SECTION outer, org=&3000 : .o EQUB 0 : SECTION inner : .p EQUB 0,0 : ENDSECTION : .q EQUB 0 : ENDSECTION") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("o")), value_make_numeric(0x3000)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("p")), value_make_numeric(0x3000)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("q")), value_make_numeric(0x3001)));
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

RC_TEST_STEP(assemble, section_guard, fix)
{
    // The guard is the first address emission must not reach: filling right up to it is fine, one
    // byte onto it is a recoverable error whose payload is the overshoot.
    RC_CHECK_TRUE(ASM("SECTION code, org=&2000, guard=&2003\nEQUB 1,2,3\nENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(ERR("SECTION code, org=&2000, guard=&2002\nEQUB 1,2,3\nENDSECTION") == error_type_guard_exceeded);
    RC_CHECK(diag_payload(&fix->r, error_type_guard_exceeded), ==, RC_STR("1"));

    // Non-halting: every offender is reported in one run.
    RC_CHECK_TRUE(ERR("SECTION a, org=0, guard=1\nEQUB 1,2\nENDSECTION\n"
                      "SECTION b, org=0, guard=1\nEQUB 1,2,3\nENDSECTION") == error_type_guard_exceeded);
    uint32_t count = 0;
    for (uint32_t i = 0; i < fix->r.diagnostics.num; i++) {
        if (rc_view_diagnostic_get(fix->r.diagnostics, i).code == error_type_guard_exceeded) {
            count++;
        }
    }
    RC_CHECK(count, ==, 2u);
}

RC_TEST_STEP(assemble, section_guard_inherits_and_converges, fix)
{
    // A nested section inherits the guard (measured against its OWN bytes; its own key overrides),
    // and a forward-referenced guard settles like any other attribute.
    RC_CHECK_TRUE(ERR("SECTION outer, org=&2000, guard=&2001\nSECTION inner\nEQUB 1,2\nENDSECTION\nENDSECTION")
                  == error_type_guard_exceeded);
    RC_CHECK_TRUE(ASM("SECTION outer, org=&2000, guard=&2001\n"
                      "SECTION inner, guard=&2002\nEQUB 1,2\nENDSECTION\nENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(ASM("SECTION code, org=0, guard=lim\nEQUB 1,2,3\nENDSECTION\nlim=3") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}


// The result's section named `name` ({0} if absent) - the INCSECTION tests read spliced buffers via it.
static section result_section(const baron_result *r, rc_str name)
{
    for (uint32_t i = 0; i < r->sections.num; i++) {
        section s = rc_view_section_get(r->sections, i);
        if (s.name.len != 0 && rc_str_is_equal(s.name, name)) {
            return s;
        }
    }
    return (section) {0};
}

// Do the named section's bytes equal `expect`? (The INCSECTION twin of code_is, which reads the default.)
static bool section_code_is(const baron_result *r, rc_str name, const uint8_t *expect, uint32_t num)
{
    section s = result_section(r, name);
    if (s.code.num != num) {
        return false;
    }
    for (uint32_t i = 0; i < num; i++) {
        if (rc_array_bytes_get(&s.code, i) != expect[i]) {
            return false;
        }
    }
    return true;
}

RC_TEST_STEP(assemble, incsection_splices_bytes, fix)
{
    // The relocation shape: `load` carries a stub, then the assembled bytes of `code` (spliced in place of
    // the reserved span by the final fixup), then a trailer. Labels around the splice measure its length -
    // the count the relocation stub needs.
    uint32_t passes = ASM("SECTION code, org=&1100\nLDA #&2A : RTS\nENDSECTION\n"
                          "SECTION load, org=&3000\nNOP\n.before\nINCSECTION code\n.after\nEQUB &FF\nENDSECTION\n"
                          "size = after - before");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("load"), (uint8_t[]) {0xEA, 0xA9, 0x2A, 0x60, 0xFF}, 5));
    RC_CHECK(result_section(&fix->r, RC_STR("load")).pc, ==, 0x3005u);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("size")), value_make_numeric(3)));

    // Splicing the same section twice is two copies.
    RC_CHECK_TRUE(ASM("SECTION a, org=0\nEQUB 1, 2\nENDSECTION\n"
                      "SECTION b, org=&2000\nINCSECTION a\nINCSECTION a\nENDSECTION") != 0);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("b"), (uint8_t[]) {1, 2, 1, 2}, 4));
}

RC_TEST_STEP(assemble, incsection_forward_reference, fix)
{
    // The source section may be defined LATER: the first pass reserves nothing (its size is unknown), the
    // settle check demands another, and the reservation then tracks the real size until the layout holds
    // still. The fixup fills the span at the very end regardless of order.
    uint32_t passes = ASM("SECTION load, org=&3000\nNOP\nINCSECTION code\nEQUB &FF\nENDSECTION\n"
                          "SECTION code, org=&1100\nLDA #&2A : RTS\nENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(passes >= 3u);   // the missed reservation forced at least one extra pass
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("load"), (uint8_t[]) {0xEA, 0xA9, 0x2A, 0x60, 0xFF}, 5));
}

RC_TEST_STEP(assemble, incsection_errors, fix)
{
    // A source that never appears is judged once everything has settled (an IF arm could still have
    // produced it), and named in the diagnostic.
    RC_CHECK_TRUE(ERR("SECTION a, org=0 : INCSECTION nosuch : ENDSECTION") == error_type_unknown_section);
    RC_CHECK(diag_payload(&fix->r, error_type_unknown_section), ==, RC_STR("nosuch"));

    // A cycle can never settle, so it is refused outright: self-insertion on the very first pass...
    RC_CHECK_TRUE(ERR("SECTION a, org=0 : NOP : INCSECTION a : ENDSECTION") == error_type_circular_incsection);
    RC_CHECK(diag_payload(&fix->r, error_type_circular_incsection), ==, RC_STR("a"));

    // ...and a mutual pair at the latest by the fixup step.
    RC_CHECK_TRUE(ERR("SECTION a, org=0 : INCSECTION b : ENDSECTION\n"
                      "SECTION b, org=&100 : INCSECTION a : ENDSECTION") == error_type_circular_incsection);

    // A missing / non-identifier name is the SECTION family's usual complaint.
    RC_CHECK_TRUE(ERR("INCSECTION") == error_type_expected_section_name);
    RC_CHECK_TRUE(ERR("INCSECTION 5") == error_type_expected_section_name);
}

RC_TEST_STEP(assemble, incsection_converges, fix)
{
    // A source whose size shifts while the assembly settles (LDA addr sizes optimistically zero-page, then
    // widens to absolute once addr binds): the reservation tracks it, and labels after the splice land on
    // the settled layout.
    uint32_t passes = ASM("SECTION load, org=&3000\nINCSECTION code\n.endlab\nENDSECTION\n"
                          "SECTION code, org=&1100\nLDA addr\nENDSECTION\n"
                          "addr = &1234");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("load"), (uint8_t[]) {0xAD, 0x34, 0x12}, 3));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("endlab")), value_make_numeric(0x3003)));
}

RC_TEST_STEP(assemble, incsection_carries_zpauto_patches, fix)
{
    // The reason the fixup runs ABSOLUTELY last: the zero-page allocator patches operand bytes after the
    // final pass, in the section the instructions emitted into. The spliced copy must carry those PATCHED
    // bytes - the allocated &70, not the placeholder 0.
#define ZP_SPLICE_PROG "ZPRESERVE &70..&7F\n" \
                       "SECTION load, org=&3000\nINCSECTION code\nENDSECTION\n" \
                       "SECTION code, org=&1100\nZPAUTO1 v\nSTA v : LDA v : RTS\nENDSECTION"
    RC_CHECK_TRUE(ASM(ZP_SPLICE_PROG) != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("load"), (uint8_t[]) {0x85, 0x70, 0xA5, 0x70, 0x60}, 5));

    // The same through the -v listing path (the listing pass rebuilds the sections; the fixup runs after).
    fix->desc.verbose = true;
    RC_CHECK_TRUE(ASM(ZP_SPLICE_PROG) != 0);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("load"), (uint8_t[]) {0x85, 0x70, 0xA5, 0x70, 0x60}, 5));
    fix->desc.verbose = false;
#undef ZP_SPLICE_PROG
}

RC_TEST_STEP(assemble, incsection_chains, fix)
{
    // A chain defined most-dependent FIRST (a needs b needs c, both forward): the dependency-ordered fixup
    // copies bottom-up, so c's ZP-patched bytes arrive in a THROUGH b.
    uint32_t passes = ASM("ZPRESERVE &70..&7F\n"
                          "SECTION a, org=0\nEQUB 1\nINCSECTION b\nENDSECTION\n"
                          "SECTION b, org=&100\nEQUB 2\nINCSECTION c\nENDSECTION\n"
                          "SECTION c, org=&200\nZPAUTO1 w\nSTA w : LDA w : RTS\nENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("c"), (uint8_t[]) {0x85, 0x70, 0xA5, 0x70, 0x60}, 5));
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("b"), (uint8_t[]) {2, 0x85, 0x70, 0xA5, 0x70, 0x60}, 6));
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("a"), (uint8_t[]) {1, 2, 0x85, 0x70, 0xA5, 0x70, 0x60}, 7));
}

RC_TEST_STEP(assemble, incsection_listing_line, fix)
{
    fix->desc.verbose = true;
    // INCSECTION lists as an address-only line: only space is reserved while the passes run - the bytes
    // belong to the post-assembly fixup - so there is no byte dump to show, like an INCLUDE or macro line.
    RC_CHECK_TRUE(ASM("SECTION code, org=&1100\nLDA #&12\nENDSECTION\n"
                      "SECTION load, org=&3000\nINCSECTION code\nENDSECTION") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("SECTION code, org=&1100\n"
                    "  1100  A9 12           LDA #&12\n"
                    "ENDSECTION\n"
                    "\n"
                    "SECTION load, org=&3000\n"
                    "  3000                  INCSECTION code\n"
                    "ENDSECTION\n"
                    "\n"));
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("load"), (uint8_t[]) {0xA9, 0x12}, 2));
    fix->desc.verbose = false;
}

RC_TEST_STEP(assemble, basic_block_emits_program, fix)
{
    // An empty block is the ROM's null program; numbered lines become full records (header, tokens,
    // patched-in length) and ENDBASIC closes with the 0D FF terminator.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("BASIC\nENDBASIC"), (uint8_t[]) {0x0D, 0xFF}, 2));
    RC_CHECK_TRUE(code_is(&fix->r, ASM("BASIC\n10PRINT\n20GOTO 10\nENDBASIC"),
                          (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xF1,
                                       0x0D, 0x00, 0x14, 0x0A, 0xE5, 0x20, 0x8D, 0x54, 0x4A, 0x40,
                                       0x0D, 0xFF}, 17));
    // Blank lines and Baron comment lines between BASIC lines vanish (the terminator coalesces
    // them), and the block closes like any statement - a ':' can follow ENDBASIC.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("BASIC\n\n; a note\n10CLS\n\nENDBASIC:NOP"),
                          (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xDB, 0x0D, 0xFF, 0xEA}, 8));
}

RC_TEST_STEP(assemble, basic_block_advances_pc, fix)
{
    // The records land in the section like any other emission, so a label after the block sits past
    // the whole program (5-byte line + 2-byte terminator).
    RC_CHECK_TRUE(ASM("BASIC\n10CLS\nENDBASIC\n.here") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("here")), value_make_numeric(7)));
}

RC_TEST_STEP(assemble, basic_dead_branch, fix)
{
    // A dead branch walks the block (to find ENDBASIC) but emits nothing and raises nothing.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF FALSE\nBASIC\n10PRINT\nENDBASIC\nENDIF\nNOP"),
                          (uint8_t[]) {0xEA}, 1));
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, basic_block_errors, fix)
{
    // Unclosed: end of input, a foreign closer, and the '}' of an enclosing scope all mean the
    // ENDBASIC never came.
    RC_CHECK_TRUE(ERR("BASIC\n10PRINT") == error_type_unclosed_basic);
    RC_CHECK_TRUE(ERR("BASIC\nENDIF") == error_type_unclosed_basic);
    RC_CHECK_TRUE(ERR("{\nBASIC\n}") == error_type_unclosed_basic);
    RC_CHECK_TRUE(ERR("ENDBASIC") == error_type_unexpected_endbasic);
    // Only numbered lines (and blanks) live inside the block - assembly statements are refused.
    RC_CHECK_TRUE(ERR("BASIC\n10REM\nLDA #42\nENDBASIC") == error_type_expected_basic_line);
    // A bad line number is recoverable: the line is dropped, the rest of the block still parses.
    RC_CHECK_TRUE(ERR("BASIC\n70000CLS\nENDBASIC") == error_type_bad_basic_line_number);
}

RC_TEST_STEP(assemble, basic_line_too_long, fix)
{
    // A single line whose record would pass 255 bytes (the length is one byte) is refused. Built
    // with an mstr because nobody wants to read a 252-character string literal.
    rc_arena build = rc_arena_make_default();
    rc_mstr  src   = rc_mstr_make(1024, &build);
    rc_mstr_append(&src, RC_STR("BASIC\n10REM"), &build);
    rc_mstr_append_n(&src, 'A', 252, &build);
    rc_mstr_append(&src, RC_STR("\nENDBASIC"), &build);
    fix->r = assemble_string(&fix->desc, RC_STR("too_long"), src.view);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_basic_line_too_long);
    rc_arena_deinit(&build);
}

RC_TEST_STEP(assemble, basic_listing, fix)
{
    fix->desc.verbose = true;
    // BASIC at the margin, then each line's whole record from its 0D, and ENDBASIC's terminator.
    RC_CHECK_TRUE(ASM("BASIC\n10CLS\n20GOTO 10\nENDBASIC") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("BASIC\n"
                    "  0000  0D 00 0A 05...  10CLS\n"
                    "  0005  0D 00 14 0A...  20GOTO 10\n"
                    "  000F  0D FF           ENDBASIC\n"));
    fix->desc.verbose = false;
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
    // `foo` is held live across `ptr`'s whole range, so they interfere: FFD places the 2-byte `ptr` first
    // (&70-&71), then the 1-byte `foo` at the next free byte, &72. (An UNUSED declaration gets no byte and
    // no definition at all - see zpauto_unused_is_warned_and_undefined.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 foo : ZPAUTO2 ptr\n"
                          "STA foo : STA ptr : LDA #&40 : STA ptr+1 : LDA (ptr),Y : ORA foo : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("ptr")), value_make_numeric(0x70)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("foo")), value_make_numeric(0x72)));

    // A comma-list declares several at once.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 p, q, r\n"
                      "STA p : STA q : STA r : LDA p : LDA q : LDA r : RTS") != 0);
    RC_CHECK_FALSE(value_is_none(baron_result_symbol(&fix->r, RC_STR("p"))));
    RC_CHECK_FALSE(value_is_none(baron_result_symbol(&fix->r, RC_STR("r"))));

    // Declared inside a named routine, a var is reachable from outside as routine.name (a dotted path).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : .routine { ZPAUTO1 v : STA v : LDA v : RTS }") != 0);
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

RC_TEST_STEP(assemble, zpauto_dotted_interface_variables, fix)
{
    // A routine's inputs and outputs declared in ITS scope, reached by the caller via the dotted path: the
    // operands attribute (and so are patched - the byte compare is the proof), the argument written before
    // the call is held live TO the call (a call consumes its callees' inputs), so the unrelated temp `t`
    // cannot be coloured over it. The result, which sub definitely writes on every path, is KILLED at the
    // call (must-write): its range starts inside sub, after both t and wid are dead, so it reuses t's byte.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 t\n"
                          "LDA #3 : STA sub.wid\n"
                          "LDA #9 : STA t\n"
                          "LDA t\n"
                          "JSR sub\n"
                          "LDA sub.res\n"
                          "RTS\n"
                          ".sub { ZPAUTO1 wid, res : LDA wid : STA res : RTS }");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "t"),       ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "sub.wid"), ==, 0x71);   // NOT t's byte: the argument survives to the JSR
    RC_CHECK(zp_addr(&fix->r, "sub.res"), ==, 0x70);   // born inside sub once t and wid are dead: reuses &70
    RC_CHECK_TRUE(code_is(&fix->r, passes,
        (uint8_t[]) {0xA9, 0x03, 0x85, 0x71,           // LDA #3 : STA sub.wid  - dotted operand PATCHED
                     0xA9, 0x09, 0x85, 0x70,           // LDA #9 : STA t
                     0xA5, 0x70,                       // LDA t
                     0x20, 0x10, 0x00,                 // JSR sub
                     0xA5, 0x70,                       // LDA sub.res           - dotted operand PATCHED
                     0x60,
                     0xA5, 0x71, 0x85, 0x70, 0x60},    // .sub: LDA wid : STA res : RTS
        21));
}

RC_TEST_STEP(assemble, zpauto_argument_survives_intermediate_call, fix)
{
    // An argument stored before TWO calls must survive the first: sub.wid is live from its store to the
    // JSR sub that consumes it, so it is live ACROSS the intervening JSR other and interferes with other's
    // whole footprint - other.loc cannot take its byte.
    uint32_t passes = ASM("ZPRESERVE &70..&7F\n"
                          "LDA #3 : STA sub.wid : JSR other : JSR sub : RTS\n"
                          ".other { ZPAUTO1 loc : STA loc : LDA loc : RTS }\n"
                          ".sub { ZPAUTO1 wid : LDA wid : RTS }");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "other.loc"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "sub.wid"),   ==, 0x71);   // pushed off loc's byte by the intervening call
}

RC_TEST_STEP(assemble, zpauto_dotted_pipeline_reuses_input_bytes, fix)
{
    // The reuse side of the same coin: two stages fed through their scoped interface variables, and the
    // whole relay runs in ONE byte. Each argument dies at its read inside its stage; each result - which
    // its stage definitely writes, so the call KILLS the pre-call value (must-write) - is born there and
    // dies at the caller's read, just as the next argument is stored. Four variables, one byte, and a
    // one-byte pool proves no interference exists anywhere.
    uint32_t passes = ASM("ZPRESERVE &70\n"
                          ".stage1 { ZPAUTO1 xin, res : LDA xin : ASL A : STA res : RTS }\n"
                          ".stage2 { ZPAUTO1 xin, res : LDA xin : CLC : ADC #7 : STA res : RTS }\n"
                          "LDA #5 : STA stage1.xin\n"
                          "JSR stage1\n"
                          "LDA stage1.res : STA stage2.xin\n"
                          "JSR stage2\n"
                          "LDA stage2.res\n"
                          "RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "stage1.xin"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "stage2.xin"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "stage1.res"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "stage2.res"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_result_survives_producer_tail, fix)
{
    // The return edge: sub's RTS sees what is live after each call to sub, so res - whose only reads are
    // in the caller - stays live from its store to the RTS, and the LATER write to t inside sub cannot
    // land on its byte. (Without the edge, res looks dead the moment it is stored, its reads being
    // invisible from inside sub, and t's store would clobber the result before the caller reads it.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F\n"
                          "JSR sub\n"
                          "LDA sub.res\n"
                          "RTS\n"
                          ".sub { ZPAUTO1 res, t : LDA #1 : STA res : LDA #2 : STA t : LDA t : RTS }");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "sub.res"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "sub.t"),   ==, 0x71);   // written after res, which is still en route out
}

RC_TEST_STEP(assemble, zpauto_address_is_typed, fix)
{
    // A ZPAUTO address is a TYPED value that exists only after allocation, so every context that needs
    // a real number NOW refuses it - eagerly, on the final pass, at the use site, naming the variable.
    // (Before the type, the placeholder 0 leaked in silently: IF deleted code, SKIP skipped nothing.)
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : SKIP v") == error_type_zpauto_address);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : SKIPTO v") == error_type_zpauto_address);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : ALIGN v") == error_type_zpauto_address);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : ZPAUTO v, q") == error_type_zpauto_address);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : IF v : NOP : ENDIF") == error_type_zpauto_address);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v\n"
                      "SECTION S, org = v : ENDSECTION") == error_type_zpauto_address);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : JSR sub : CANCALL v : RTS\n"
                      ".sub RTS") == error_type_zpauto_address);

    // The only arithmetic an address supports is +/- an integer; everything else refuses through the
    // evaluator's ordinary type checks. A comparison, a multiply, a range endpoint - all caught.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : IF v = 1 : NOP : ENDIF") == error_type_type_mismatch);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : EQUB v * 2") == error_type_type_mismatch);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v\n"
                      "FOR n = v..8 : NEXT") == error_type_type_mismatch);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v, q : STA v : LDA v : STA q : LDA q\n"
                      "EQUB v - q") == error_type_domain);   // two different bases have no knowable distance
}

RC_TEST_STEP(assemble, zpauto_address_arithmetic, fix)
{
    // The affine cases still work exactly as before: an offset rides the value, and the output pass
    // emits base+offset. The pointer idiom's bytes are the proof.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO2 p : STA p : STA p+1 : LDA (p),Y : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0x85, 0x70, 0x85, 0x71, 0xB1, 0x70, 0x60}, 7));

    // The distance between two offsets into the SAME variable is a plain number.
    RC_CHECK_TRUE(code_is(&fix->r,
        ASM("ZPRESERVE &70..&7F : ZPAUTO2 p : STA p : STA p+1 : LDA (p),Y : EQUB (p+2)-p"),
        (uint8_t[]) {0x85, 0x70, 0x85, 0x71, 0xB1, 0x70, 0x02}, 7));
}

RC_TEST_STEP(assemble, zpauto_address_tables_work, fix)
{
    // Data emission accepts an address: element widths are fixed, so the layout cannot depend on the
    // value, and the output pass emits the ALLOCATED address - address tables of variables just work.
    // (Before the type, EQUB var silently emitted the placeholder 0 into the real output.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : EQUB v : EQUW v+1");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0x85, 0x70, 0xA5, 0x70, 0x70, 0x71, 0x00}, 7));
}

RC_TEST_STEP(assemble, zpauto_immediate_address_works, fix)
{
    // An immediate accepts an address too - the way a pointer is seeded with a variable's location.
    // (Before the type, LDA #var silently emitted #0: immediates are never attributed or patched.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : ZPAUTO1 v\n"
                          "LDA #v : STA ptr : LDA #0 : STA ptr+1\n"
                          "STA v : LDA v\n"
                          "LDA (ptr),Y : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "ptr"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "v"),   ==, 0x72);
    RC_CHECK_TRUE(code_is(&fix->r, passes,
        (uint8_t[]) {0xA9, 0x72, 0x85, 0x70, 0xA9, 0x00, 0x85, 0x71,
                     0x85, 0x72, 0xA5, 0x72, 0xB1, 0x70, 0x60}, 15));
}

RC_TEST_STEP(assemble, zpauto_alias_attributes, fix)
{
    // The value CARRIES the variable's identity, so an alias attributes like the variable itself -
    // the instruction joins liveness and emits the real address. (Before, `LDA x` emitted &00: the
    // lex-based attribution saw only `x`, whose binding is an assignment, not a variable.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : x = v : STA x : LDA x : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]) {0x85, 0x70, 0xA5, 0x70, 0x60}, 5));
    // The output pass re-binds the alias against the real address, so the result symbol is honest.
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("x")), value_make_numeric(0x70)));

    // Offset arithmetic rides through the alias too, and the derived binding CONVERGES (the zpauto
    // equality case) - a hi-byte alias of a pointer works end to end.
    uint32_t q = ASM("ZPRESERVE &70..&7F : ZPAUTO2 p : hi = p + 1 : STA p : STA hi : LDA (p),Y : RTS");
    RC_CHECK_TRUE(q != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(code_is(&fix->r, q, (uint8_t[]) {0x85, 0x70, 0x85, 0x71, 0xB1, 0x70, 0x60}, 7));
}

RC_TEST_STEP(assemble, zpauto_print_allocated_address, fix)
{
    // PRINT speaks on the output pass, where the symbol holds its allocated address - in EVERY mode
    // (before, a non-verbose PRINT printed the placeholder 0 while -v printed the real byte).
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : PRINT v");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(rc_str_is_equal(fix->r.channels[0], RC_STR("112\n")));   // &70, in PRINT's decimal
}

RC_TEST_STEP(assemble, zpauto_print_hex_address, fix)
{
    // '~' is the reason PRINT can speak the BBC's language, and a ZPAUTO address is its headline case:
    // the placeholder must reach it without complaint on the earlier passes (a refusal there would be
    // reported at the final pass, where PRINT is still silent) and come out as the allocated byte.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : STA ptr : STA ptr+1\n"
                          "PRINT \"ptr at &\", ~ptr, \", high byte &\", ~ptr+1");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(rc_str_is_equal(fix->r.channels[0], RC_STR("ptr at &70, high byte &71\n")));
}

RC_TEST_STEP(assemble, zpauto_jmp_via_variable_vector, fix)
{
    // A JMP through a ZPAUTO pointer is a dispatch through a cell WE own: computed flow, refused
    // without a CANJUMP naming the arms. (Before, the placeholder vector &0000 read as an external
    // OS cell - a clean exit - and the emitted operand was never patched.)
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO2 vec : STA vec : STA vec+1 : JMP (vec)\n"
                      ".h RTS") == error_type_zpauto_computed_flow);

    // Annotated, it works: the edge is wired, the vector stays LIVE to the jump (the dispatch reads
    // the pair), and the emitted operand carries the allocated cell.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO2 vec\n"
                          "LDA #LO(h) : STA vec\n"
                          "LDA #HI(h) : STA vec+1\n"
                          "JMP (vec)\n"
                          "CANJUMP h\n"
                          ".h RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "vec"), ==, 0x70);
    RC_CHECK_TRUE(code_is(&fix->r, passes,
        (uint8_t[]) {0xA9, 0x0B, 0x85, 0x70, 0xA9, 0x00, 0x85, 0x71, 0x6C, 0x70, 0x00, 0x60}, 12));

    // A one-byte variable cannot hold a two-byte vector - the pointer-width check applies here too.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 vec : STA vec : JMP (vec) : CANJUMP h\n"
                      ".h RTS") == error_type_zpauto_narrow_pointer);
}

RC_TEST_STEP(assemble, cmos_section_enables, fix)
{
    // cmos = TRUE opens the whole 65C02 set for the section: new mnemonics, and the CMOS-only modes on
    // NMOS mnemonics - (zp) indirect, BIT #imm, INC/DEC A, and the indexed dispatch JMP (abs,X).
    uint32_t passes = ASM("SECTION C, cmos = TRUE\n"
                          "PHX : PLY : INA\n"
                          "STZ &70\n"
                          "TRB &70\n"
                          "BRA over\n"
                          ".over\n"
                          "LDA (&70)\n"
                          "BIT #1\n"
                          ".tbl EQUW over\n"
                          "JMP (tbl,X)\n"
                          "ENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("C"),
        (uint8_t[]) {0xDA, 0x7A, 0x1A,                 // PHX : PLY : INA
                     0x64, 0x70,                       // STZ &70
                     0x14, 0x70,                       // TRB &70
                     0x80, 0x00,                       // BRA over (a jump in the CFG: no fall-through edge)
                     0xB2, 0x70,                       // LDA (&70) - the CMOS zero-page indirect
                     0x89, 0x01,                       // BIT #1
                     0x09, 0x00,                       // .tbl EQUW over
                     0x7C, 0x0D, 0x00},                // JMP (tbl,X) - indexed dispatch
        18));
}

RC_TEST_STEP(assemble, cmos_bit_indexed, fix)
{
    // BIT zp,X and BIT abs,X are 65C02 additions too, gated exactly like the other CMOS-only modes
    // (they were missing their cmos flag until now).
    uint32_t passes = ASM("SECTION C, cmos = TRUE\n"
                          "BIT &70,X\n"
                          "BIT &1234,X\n"
                          "ENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("C"),
        (uint8_t[]) {0x34, 0x70,                       // BIT &70,X
                     0x3C, 0x34, 0x12},                // BIT &1234,X
        5));

    RC_CHECK_TRUE(ERR("BIT &70,X") == error_type_needs_cmos);
    RC_CHECK_TRUE(ERR("BIT &1234,X") == error_type_needs_cmos);
}

RC_TEST_STEP(assemble, cmos_refused_outside, fix)
{
    // Without the attribute (the default section included) every CMOS encoding refuses with its own
    // message - the encoding exists, one attribute away, which is worth saying.
    RC_CHECK_TRUE(ERR("PHX") == error_type_needs_cmos);
    RC_CHECK_TRUE(ERR("STZ &70") == error_type_needs_cmos);
    RC_CHECK_TRUE(ERR("LDA (&70)") == error_type_needs_cmos);
    RC_CHECK_TRUE(ERR("SECTION S : BRA out : .out ENDSECTION") == error_type_needs_cmos);

    // A genuinely impossible shape is still the plain bad-mode error, cmos or not.
    RC_CHECK_TRUE(ERR("LDX (&70)") == error_type_bad_addressing_mode);
    RC_CHECK_TRUE(ERR("SECTION C, cmos = TRUE : LDX (&70) : ENDSECTION") == error_type_bad_addressing_mode);
}

RC_TEST_STEP(assemble, cmos_inherits_and_overrides, fix)
{
    // Nested sections inherit the flag like any attribute; an explicit cmos = FALSE opts back out; a
    // sibling section is untouched by either.
    uint32_t passes = ASM("SECTION outer, cmos = TRUE\n"
                          "SECTION inner : PHX : ENDSECTION\n"
                          "ENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);

    RC_CHECK_TRUE(ERR("SECTION outer, cmos = TRUE\n"
                      "SECTION inner, cmos = FALSE : PHX : ENDSECTION\n"
                      "ENDSECTION") == error_type_needs_cmos);

    RC_CHECK_TRUE(ERR("SECTION a, cmos = TRUE : PHX : ENDSECTION\n"
                      "SECTION b : PHX : ENDSECTION") == error_type_needs_cmos);
}

RC_TEST_STEP(assemble, cmos_forward_reference_converges, fix)
{
    // The flag is an ordinary attribute expression: a forward-referenced value owes a pass and settles.
    uint32_t passes = ASM("SECTION C, cmos = flag : PHX : ENDSECTION\n"
                          "flag = TRUE");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(section_code_is(&fix->r, RC_STR("C"), (uint8_t[]) {0xDA}, 1));
}

RC_TEST_STEP(assemble, cmos_zpauto_interplay, fix)
{
    // The zero-page analyses already model the CMOS shapes; with the gate open they take part:
    // STZ is a write-only def, so it KILLS - w's old value ends at the STZ and v can share its byte.
    uint32_t passes = ASM("SECTION C, cmos = TRUE\n"
                          "ZPRESERVE &70..&7F : ZPAUTO1 v, w\n"
                          "STA w : LDA w\n"
                          "STZ w\n"
                          "STA v : LDA v : RTS\n"
                          "ENDSECTION");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "w"), ==, 0x70);

    // The (zp) indirect dereferences a pointer pair: fine on a ZPAUTO2, the usual refusal on a ZPAUTO1.
    RC_CHECK_TRUE(ASM("SECTION C, cmos = TRUE\n"
                      "ZPRESERVE &70..&7F : ZPAUTO2 p\n"
                      "STA p : STA p+1 : LDA (p) : RTS\n"
                      "ENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(ERR("SECTION C, cmos = TRUE\n"
                      "ZPRESERVE &70..&7F : ZPAUTO1 q\n"
                      "STA q : LDA (q) : RTS\n"
                      "ENDSECTION") == error_type_zpauto_narrow_pointer);
}

RC_TEST_STEP(assemble, zpauto_conditional_result_is_preserved, fix)
{
    // The must-write kill needs EVERY path to write: here the store is skipped when the sum is zero, so the
    // caller's read may see the byte's pre-call value - res is genuinely live across its own call and is
    // kept clear of the routine's workspace (no sharing with xin), unlike the unconditional store above.
    uint32_t passes = ASM("ZPRESERVE &70..&7F\n"
                          ".offset { ZPAUTO1 xin, res : LDA xin : CLC : ADC #7 : BEQ @+ : STA res : .@ RTS }\n"
                          "LDA #5 : STA offset.xin\n"
                          "JSR offset\n"
                          "LDA offset.res\n"
                          "RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "offset.xin"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "offset.res"), ==, 0x71);   // conditionally written: preserved, not reborn
}

RC_TEST_STEP(assemble, zpauto_allocation_refusals, fix)
{
    // The certainty contract: rather than emit code it cannot prove correct, the allocator errors and points
    // at the fix. Each of these MUST fail the assemble.

    // A variable FRESHLY written (STA cnt) then held live across the recursive call is a per-level value: each
    // level wants its own byte, which one static address cannot give. (Merely reading / accumulating it across
    // the recursion is fine - see zpauto_recursion_shared_vs_per_level.)
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : .r { ZPAUTO1 cnt : STA cnt : JSR r : LDA cnt : RTS }")
                  == error_type_zpauto_recursion);

    // A computed / indirect jump reaches code the CFG cannot follow while a variable is in play - refuse and
    // ask for an annotation. The vector must be one of OUR labels: a cell we assembled holds a run-time value
    // that may point back into our own code. (A CONSTANT vector - JMP (&FFFC) - is a fixed OS vector and
    // reads as a clean exit instead; see zpauto_external_calls_and_jumps.)
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : JMP (vec) : .vec EQUW &2000")
                  == error_type_zpauto_computed_flow);

    // More simultaneously-live variables than reserved bytes is a spill: p and q overlap but only &70 is free.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 p, q : STA p : STA q : LDA p : LDA q : RTS")
                  == error_type_zeropage_full);
}

RC_TEST_STEP(assemble, zpauto_recursion_shared_vs_per_level, fix)
{
    // Recursion is only fatal for a value the cycle FRESHLY writes and then needs back after the child returns.
    // A value merely read or accumulated (DEC/INC) across the recursion rides on one shared byte quite happily -
    // it is a single running counter, not a distinct value per frame. So this shape is ALLOWED: `n` is seeded by
    // the caller (outside the recursion) and only ever DEC'd inside `down`, while `keep` sits live across the
    // whole descent. Neither is freshly assigned within the cycle, so allocation proceeds; because both are live
    // across the call they interfere with `down`'s footprint and take distinct bytes.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep, n\n"
                      "LDA #10 : STA keep : LDA #5 : STA n : JSR down\n"
                      "LDA keep : CLC : ADC n : RTS\n"
                      ".down { DEC n : BEQ done : JSR down : .done RTS }") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);   // recursion no longer refused outright
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "n"),    ==, 0x71);   // forced off keep's byte by the live-across recursion

    // Contrast (the "would infinitely allocate" case, REFUSED): `level` is written afresh at every frame and
    // read back after the child returns, so each recursion level genuinely needs its OWN byte - unbounded in a
    // fixed zero page. This is the shape a real per-level temp (a factorial accumulator, a saved cursor) takes.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : .descend { ZPAUTO1 level : STA level : JSR descend : LDA level : RTS }")
                  == error_type_zpauto_recursion);

    // The same discrimination holds for MUTUAL recursion: the footprint walk spans the whole cycle (a calls b,
    // b calls a), so a fresh write anywhere in it is seen. Per-level `v` (STA in a, live across a's JSR b) is
    // refused; a shared counter DEC'd across the same cycle is allowed.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : .a { ZPAUTO1 v : STA v : JSR b : LDA v : RTS } : .b { JSR a : RTS }")
                  == error_type_zpauto_recursion);
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep, n\n"
                      "LDA #10 : STA keep : LDA #5 : STA n : JSR a : LDA keep : CLC : ADC n : RTS\n"
                      ".a { DEC n : BEQ done : JSR b : .done RTS } : .b { JSR a : RTS }") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, zpauto_indexed_access_warns, fix)
{
    // Indexed / indexed-indirect access into an auto-variable (var,X / var,Y / (var,X)) touches var+index at
    // run time - the intended way to walk a ZPAUTO table. The allocator reserves the whole variable and marks
    // it all live on the access, so an index within its width is sound; the run-time index itself is unprovable,
    // so it is an OPT-IN warning (silent until the level is raised), not a refusal. The variable still allocates.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO 4, table : STA table : LDA table,X : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);              // it assembles: no error-severity diagnostic
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_indexed_access));  // ... but the opt-in warning is recorded
    RC_CHECK(zp_addr(&fix->r, "table"), ==, 0x70);                       // and the table is placed as usual

    // `v,Y` (widens to absolute indexed, no zp form) and `(p,X)` (indexed-indirect) are likewise warned, not refused.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO 4, v : STA v : LDA v,Y : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_indexed_access));
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO 4, p : STA p : LDA (p,X) : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_indexed_access));

    // The CONSTANT base of an indexed access is still bounds-checked (Guard 0b): `t+4,X` on a 4-wide table is
    // refused because the base is already off the end, before any index is added.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO 4, t : LDA t+4,X : RTS") == error_type_zpauto_out_of_bounds);

    // Pointer width is unchanged: a 1-byte ZPAUTO1 dereferenced as a pointer ((v),Y / (v)) is still refused -
    // that is a compile-time-known 2-byte access off a 1-byte variable, nothing to do with a run-time index.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : LDA (v),Y") == error_type_zpauto_narrow_pointer);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : STA (v),Y") == error_type_zpauto_narrow_pointer);

    // The envelope-safe forms stay legal AND raise no warning: direct `var`, the `var+1` hi byte, and the
    // whole-pointer `(var),Y` dereference of a 2-byte ZPAUTO2 (only the DATA is indexed by Y; the pointer is direct).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_zpauto_indexed_access));
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 p : STA p : STA p+1 : LDA (p),Y") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_zpauto_indexed_access));
}

RC_TEST_STEP(assemble, zpauto_generic_width, fix)
{
    // ZPAUTO <n>, names declares n-byte variables (a table / struct); ZPAUTO1 / ZPAUTO2 are the 1/2-byte sugar.
    // A 4-byte table takes 4 consecutive reserved bytes; a 1-byte var that interferes packs after it, at +4.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO 4, table : ZPAUTO1 flag\n"
                      "STA table : STA table+3 : LDA flag : LDA table : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "table"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "flag"),  ==, 0x74);   // forced past the 4-byte table's span

    // The count is an ordinary constant expression.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO 2+2, big : STA big+3 : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);

    // Bounds: var+n within the width is fine; reaching past the end is refused (the byte belongs to nobody /
    // a neighbour). This also now catches a ZPAUTO1 `var+1` hi-byte access that used to slip through silently.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO 4, t : STA t+4 : RTS") == error_type_zpauto_out_of_bounds);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 v : LDA v+1 : RTS")   == error_type_zpauto_out_of_bounds);

    // A pointer needs 2 bytes AT THE OFFSET, not exactly a ZPAUTO2: the first two bytes of a wider table work.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO 3, t : STA t : STA t+1 : LDA (t),Y : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    // ... but a pointer straddling the top of the table is out of bounds ((t+2),Y reads t+2, t+3 of a 3-wide t).
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO 3, t : LDA (t+2),Y : RTS") == error_type_zpauto_out_of_bounds);

    // A count outside 1..256 is rejected.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO 0, x : STA x")   == error_type_zpauto_bad_width);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO 300, x : STA x") == error_type_zpauto_bad_width);
}

RC_TEST_STEP(assemble, zpauto_layout_must_not_collide, fix)
{
    // The flow analysis identifies a block by (section, pc), so two sections sharing an address are distinct.
    // The ONLY collision left is two instructions at one address WITHIN a section (an org rewind).

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

    // The paged-bank case: two sections loaded to the SAME address (coexisting banks) now coexist under ZPAUTO
    // - the (section, pc) key tells the two banks apart, so this is no longer refused. Each bank's variable
    // reuses the same reserved byte, since no flow connects the banks.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v, w\n"
                      "SECTION a, org=&8000 : STA v : LDA v : RTS : ENDSECTION\n"
                      "SECTION b, org=&8000 : STA w : LDA w : RTS : ENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "w"), ==, 0x70);   // same byte: the two banks never interfere

    // (There is no way to place two instructions at one address WITHIN a section: a section's org is fixed at
    // open and its cursor only advances - by emission or a forward SKIP/SKIPTO/ALIGN - so its pc is strictly
    // monotonic. cfg_build asserts that invariant in debug builds; there is no same-section-collision error.)
}

RC_TEST_STEP(assemble, zpauto_var_live_across_cross_section_call, fix)
{
    // A variable held live across a JSR into ANOTHER section is fully supported. The call resolves by the
    // target LABEL (`sub` lives in `bank`, so the edge crosses the section - a bare address never could), the
    // callee's footprint is computed across the boundary, and the live-across variable is kept clear of it.
    // `keep` is written, then read AFTER the call into `bank`; `bank`'s routine touches `tmp`. So keep is live
    // across the call, must interfere with tmp, and takes a DIFFERENT byte rather than sharing one.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep, tmp\n"
                      "SECTION main, org=&1900 : STA keep : JSR sub : LDA keep : RTS : ENDSECTION\n"
                      "SECTION bank, org=&8000 : .sub : STA tmp : LDA tmp : RTS : ENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(zp_addr(&fix->r, "keep") != zp_addr(&fix->r, "tmp"));   // live across the call -> disjoint

    // For contrast: with NOTHING live across the same cross-section call, keep and tmp DO share a byte - the
    // interference only exists because a value spanned the call, not because the call crosses a section.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep, tmp\n"
                      "SECTION main, org=&1900 : STA keep : LDA keep : JSR sub : RTS : ENDSECTION\n"
                      "SECTION bank, org=&8000 : .sub : STA tmp : LDA tmp : RTS : ENDSECTION") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, zp_addr(&fix->r, "tmp"));   // nothing spans the call -> reuse
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
    // An indirect JMP through a vector WE assembled is refused with a variable live across it: the cell's
    // run-time contents may point back into our own code, so the CFG cannot prove `keep` survives. (Only an
    // in-program vector is computed flow - a constant vector cell like JMP (&FFFC) lies outside the program
    // and reads as a clean exit; see zpauto_external_calls_and_jumps.)
    RC_CHECK_TRUE(ERR("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                      "STA keep : JMP (vector)\n"
                      ".hA : LDA keep : RTS\n"
                      ".hB : LDA keep : RTS\n"
                      ".vector EQUW hA") == error_type_zpauto_computed_flow);

    // CANJUMP declares the jump table's targets, so the CFG wires each as a real successor edge: keep is live
    // into both arms and allocates cleanly onto &70.
    uint32_t p = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                     "STA keep : JMP (vector) : CANJUMP hA, hB\n"
                     ".hA : LDA keep : RTS\n"
                     ".hB : LDA keep : RTS\n"
                     ".vector EQUW hA");
    RC_CHECK_TRUE(p != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, 0x70);

    // A declared destination may itself be EXTERNAL: here one arm of the dispatch leaves the program for the
    // OS. That arm reads as a clean exit (external code touches none of our variables), the in-program arm is
    // wired as usual, and keep still allocates. This is how you tell Baron a vector you own can hold an
    // external address - no separate annotation needed.
    uint32_t q = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                     "STA keep : JMP (vector) : CANJUMP hA, &FFEE\n"
                     ".hA : LDA keep : RTS\n"
                     ".vector EQUW hA");
    RC_CHECK_TRUE(q != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpauto_rts_dispatch_canjump, fix)
{
    // The RTS-dispatch trick: push a target address minus one, RTS into it. CANJUMP after the RTS marks it
    // as the jump it really is, wiring the declared edge - so v, consumed by the dispatch target, is live
    // through the pushes and the unrelated temp w cannot take its byte. (Unannotated, an RTS-dispatch is
    // indistinguishable from a real return and remains a trusted precondition, like a wrong UNREACHABLE.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 v, w\n"
                          "STA v\n"
                          "LDA #HI(target-1) : PHA\n"
                          "LDA #LO(target-1) : PHA\n"
                          "STA w : LDA w\n"
                          "RTS\n"
                          "CANJUMP target\n"
                          ".target : LDA v : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "w"), ==, 0x71);   // v is live across the pushes: no sharing

    // The dispatch composes with the call analyses: a routine that RTS-dispatches into a helper has the
    // helper in its extent, so a caller variable live across the JSR is kept off the helper's local, and
    // the routine's returning exit is the HELPER's RTS, not the dispatch.
    uint32_t q = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                     "STA keep : JSR disp : LDA keep : RTS\n"
                     ".disp { LDA #HI(helper-1) : PHA : LDA #LO(helper-1) : PHA : RTS : CANJUMP helper }\n"
                     ".helper { ZPAUTO1 loc : STA loc : LDA loc : RTS }");
    RC_CHECK_TRUE(q != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"),       ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "helper.loc"), ==, 0x71);   // reached through the dispatch edge

    // An external arm - RTS-dispatching into the OS - is a clean exit that returns to our caller through
    // the OS routine's own RTS, so keep survives it with nothing to dodge.
    uint32_t r = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                     "STA keep : JSR disp : LDA keep : RTS\n"
                     ".disp { LDA #HI(&FFEE-1) : PHA : LDA #LO(&FFEE-1) : PHA : RTS : CANJUMP &FFEE }");
    RC_CHECK_TRUE(r != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, 0x70);

    // The PHP:RTI flavour (address pushed unadjusted, RTI pops the status byte first) is the same flow
    // class as RTS, so the annotation works identically.
    uint32_t s = ASM("ZPRESERVE &70..&7F : ZPAUTO1 v, w\n"
                     "STA v\n"
                     "LDA #HI(target) : PHA : LDA #LO(target) : PHA : PHP\n"
                     "STA w : LDA w\n"
                     "RTI\n"
                     "CANJUMP target\n"
                     ".target : LDA v : RTS");
    RC_CHECK_TRUE(s != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "w"), ==, 0x71);
}

RC_TEST_STEP(assemble, zpauto_partial_write_tracks_bytes, fix)
{
    // The demo.6502 bug distilled: ptr's MSB is set once up front; the loop rewrites only the LSB before
    // each deref. That store redefines one byte - the MSB flows through it - so ptr must stay live around
    // the loop and the loop temp may not overlap it. (Before per-byte liveness the LSB store read as a
    // full kill, tmp landed on ptr's bytes, and the MSB was clobbered at run time.)
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : ZPAUTO1 tmp\n"
                          "LDA #&39 : STA ptr+1\n"
                          ".loop\n"
                          "LDA #0 : STA ptr\n"
                          "LDA (ptr),Y : STA tmp : LDA tmp\n"
                          "DEX : BNE loop\n"
                          "RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "ptr"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "tmp"), ==, 0x72);   // forced off BOTH of ptr's bytes

    // The flip side: partial writes accumulate across the width, so a pointer FULLY rewritten - both
    // bytes, one store each - genuinely dies at the rewrite, and a temp that expires beforehand still
    // shares its bytes.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr : ZPAUTO1 tmp\n"
                      "STA tmp : LDA tmp\n"
                      "STA ptr : LDA #&39 : STA ptr+1\n"
                      "LDA (ptr),Y : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "ptr"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "tmp"), ==, 0x70);   // dead before the rewrite begins - reuse is safe
}

RC_TEST_STEP(assemble, zpauto_unused_is_warned_and_undefined, fix)
{
    // A ZPAUTO no instruction touches is warned (severity_warning - shown by default), given NO address,
    // and its binding REMOVED - as if the declaration were not there. `gap` sits between two used vars:
    // with it skipped, y packs at &71 (previously gap pinned &71 to itself and pushed y to &72).
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 x, gap, y\n"
                          "STA x : STA y : LDA x : LDA y : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);   // a warning, not a refusal
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_unused));
    RC_CHECK(zp_addr(&fix->r, "x"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "y"), ==, 0x71);                // gap took nothing
    RC_CHECK_TRUE(value_is_none(baron_result_symbol(&fix->r, RC_STR("gap"))));   // and is not defined

    // A fully used trio raises no such warning.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 x, y\n"
                      "STA x : STA y : LDA x : LDA y : RTS") != 0);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_zpauto_unused));

    // An unused declaration inside a FOR body is one warning, not one per iteration (the instantiations
    // share a def cursor).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : FOR i = 1..8 : ZPAUTO1 spare : NEXT : RTS") != 0);
    uint32_t warnings = 0;
    for (uint32_t i = 0; i < fix->r.diagnostics.num; i++) {
        if (rc_view_diagnostic_get(fix->r.diagnostics, i).code == error_type_zpauto_unused) {
            warnings++;
        }
    }
    RC_CHECK(warnings, ==, 1u);
}

RC_TEST_STEP(assemble, zpauto_external_calls_and_jumps, fix)
{
    // A call or jump to a CONSTANT destination that matches nothing we assembled leaves the program - an OS
    // or ROM entry. ZPRESERVE names precisely the bytes nothing outside the program uses, so external code
    // cannot touch a ZPAUTO: the call contributes an EMPTY footprint and needs no CANCALL. Here `keep` rides
    // straight across JSR &FFEE (OSWRCH) and still allocates.
    uint32_t passes = ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                          "STA keep : JSR &FFEE : LDA keep : RTS");
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"), ==, 0x70);

    // The idiomatic named OS entry is the same thing: a `name = expr` constant is not a code label, so the
    // destination still reads as external.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : oswrch = &FFEE : ZPAUTO1 keep\n"
                      "STA keep : JSR oswrch : LDA keep : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);

    // An indirect JMP through a CONSTANT vector cell (JMP (&FFFC)) dispatches through memory outside the
    // program: wherever it lands is external code, so the block is a clean exit, not computed flow.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : JMP (&FFFC)") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);

    // A direct tail-jump out of the program is likewise a clean exit...
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v : STA v : LDA v : JMP &FFEE") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);

    // ...and a callee ENDING in an external transfer still has a bounded footprint: sub touches only its own
    // loc (the tail JMP &FFEE contributes nothing), so keep - live across the JSR - is forced off loc's byte
    // and both allocate.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep\n"
                      "STA keep : JSR sub : LDA keep : RTS\n"
                      ".sub { ZPAUTO1 loc : STA loc : LDA loc : JMP &FFEE }") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "keep"),    ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "sub.loc"), ==, 0x71);
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
    baron_desc desc = (baron_desc) {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };

    // Straight-line: STA is a write, LDA a read, INC a read-modify-write; all name the one variable (vreg 0).
    {
        baron b = baron_make(&desc);
        uint32_t s = source_files_add_string(&b.source_files, RC_STR("t1"),
            RC_STR("ZPRESERVE &70..&7F : ZPAUTO1 foo : STA foo : LDA foo : INC foo"));
        run_passes(&b, s, desc.scratch);
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
        baron b = baron_make(&desc);
        uint32_t s = source_files_add_string(&b.source_files, RC_STR("t2"),
            RC_STR("ZPRESERVE &70..&7F : ZPAUTO2 ptr : STA ptr : STA ptr+1 : LDA (ptr),Y : STA (ptr),Y"));
        run_passes(&b, s, desc.scratch);
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
        baron b = baron_make(&desc);
        uint32_t s = source_files_add_string(&b.source_files, RC_STR("t3"),
            RC_STR("ZPRESERVE &70..&7F : label = &50 : LDA label : LDA &2000 : LDA #7"));
        run_passes(&b, s, desc.scratch);
        RC_CHECK(zeropage_insn_count(&b.zeropage), ==, 3u);   // all three LDAs are recorded...
        for (uint32_t i = 0; i < 3; i++) {
            RC_CHECK(zeropage_insn_get(&b.zeropage, i).vreg, ==, RC_INDEX_NONE);   // ...but touch no vreg
        }
    }

    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
}

RC_TEST(assemble, zpauto_liveness_end_to_end)
{
    // The whole Stage C chain over REAL assembler output: parse the spec's `mul` routine, take the recorded
    // IR straight off b.zeropage, build the CFG and run liveness, and confirm the two facts the analysis
    // exists to establish - the in/out/temp classification, and the byte-reuse (out1 reuses in1's / tmp's
    // range because their live ranges are disjoint). vreg ids follow declaration order: in1=0, tmp=1, out1=2.
    baron_desc desc = (baron_desc) {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };
    rc_arena arena = rc_arena_make_default();     // holds the cfg + liveness result
    rc_arena scratch = rc_arena_make_default();   // distinct by-value scratch for the analysis

    baron b = baron_make(&desc);
    uint32_t s = source_files_add_string(&b.source_files, RC_STR("mul"),
        RC_STR("ZPRESERVE &70..&7F : ZPAUTO1 in1, tmp, out1\n"
               ".mul { LDA in1 : ASL A : STA tmp : LDA in1 : CLC : ADC tmp : STA out1 : RTS }"));
    run_passes(&b, s, desc.scratch);

    cfg g = cfg_build(zeropage_insns(&b.zeropage), zeropage_cflows(&b.zeropage), zeropage_labels(&b.zeropage),
                      zeropage_entries(&b.zeropage), &arena, scratch);
    liveness lv = liveness_analyze(g, zeropage_insns(&b.zeropage), zeropage_cflows(&b.zeropage),
                                   zeropage_vars(&b.zeropage), 0, &arena, scratch);

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
    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
}

RC_TEST_STEP(assemble, zpentry_parses_and_is_reserved, fix)
{
    // The marker emits nothing, so it records the label's pc whichever side of the label it sits.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70 : ZPAUTO1 v : .main : ZPENTRY : STA v : LDA v : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_zpauto_unreachable));
    RC_CHECK_TRUE(ASM("ZPRESERVE &70 : ZPAUTO1 v : ZPENTRY : .main : STA v : LDA v : RTS") != 0);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_zpauto_unreachable));

    // The names are statement keywords now, so a label cannot be spelled after them...
    RC_CHECK_TRUE(ERR(".zpentry RTS") == error_type_expected_label_name);
    // ...and a marker takes no operand.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPENTRY 5 : RTS") == error_type_expected_separator);

    // Without ZPRESERVE the feature is off and the marker is inert, like the other annotations.
    RC_CHECK_TRUE(ASM("ZPENTRY : LDA #1 : RTS") != 0);
    RC_CHECK(fix->r.diagnostics.num, ==, 0u);
}

RC_TEST_STEP(assemble, zpentry_marks_no_code_errors, fix)
{
    // A marker at a pc where no instruction starts declares nothing - the end of the code...
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 v : .main STA v : LDA v : RTS : ZPENTRY")
                  == error_type_zpentry_no_code);
    // ...or a run of data (EQUB records no instruction).
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 v : .main STA v : LDA v : RTS\nZPENTRY : EQUB 1")
                  == error_type_zpentry_no_code);

    // Mid-routine the marker is legal but marks THAT pc, so the prefix above it warns - self-diagnosing.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70 : ZPAUTO1 v : .main STA v : ZPENTRY : LDA v : RTS") != 0);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_unreachable));
}

RC_TEST_STEP(assemble, zpentry_replaces_default_roots, fix)
{
    // Two routines, nothing calling the second. The default root is the section's first block, so only
    // `other` is off the map.
    #define TWO_ROUTINES(markers1, markers2) \
        "ZPRESERVE &70..&7F : ZPAUTO1 u, w\n" \
        ".main " markers1 "STA u : LDA u : RTS\n" \
        ".other " markers2 "STA w : LDA w : RTS\n"
    RC_CHECK_TRUE(ASM(TWO_ROUTINES("", "")) != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 1u);

    // One ZPENTRY replaces the default: only the marked routine roots, so `other` still warns.
    RC_CHECK_TRUE(ASM(TWO_ROUTINES("ZPENTRY : ", "")) != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 1u);

    // Marking both covers everything.
    RC_CHECK_TRUE(ASM(TWO_ROUTINES("ZPENTRY : ", "ZPENTRY : ")) != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);
    #undef TWO_ROUTINES
}

RC_TEST_STEP(assemble, zpentry_default_roots_per_section, fix)
{
    // With no markers anywhere, EACH section's first block roots itself - the multi-section generalisation
    // of the old block-0 presumption, and what keeps the INCSECTION relocation workflow warning-free.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 u, w\n"
                      "SECTION one, org=&2000\n.m1 STA u : LDA u : RTS\nENDSECTION\n"
                      "SECTION two, org=&3000\n.m2 STA w : LDA w : RTS\nENDSECTION\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);
}

RC_TEST_STEP(assemble, zpinterrupt_keeps_default_roots, fix)
{
    // A handler declaration says nothing about where the mainline starts, so the defaults stay: main is
    // rooted by its section, the handler by its marker - no warnings from either.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq ZPINTERRUPT : STA h : LDA h : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);
}

RC_TEST_STEP(assemble, zpauto_unreachable_island_warns, fix)
{
    // The shape this whole feature exists to catch: an interrupt handler nothing calls. Its variables are
    // analysed as a disconnected island, so the layout around them is a guess - warn, once.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq STA h : LDA h : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);   // a warning, not an error
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 1u);
}

RC_TEST_STEP(assemble, zpauto_unreachable_region_dedup, fix)
{
    // A multi-block island (branch + join) is ONE region: the head speaks once for all of it.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq STA h : BNE done : LDA h\n.done LDA h : RTI\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 1u);
}

RC_TEST_STEP(assemble, zpauto_unreachable_follows_calls, fix)
{
    // Reachability walks call targets and CANJUMP-wired dispatch arms, so a routine only ever entered
    // through a JSR or a declared jump table is on the map - no warning.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      ".main JSR sub : RTS\n"
                      ".sub STA v : LDA v : RTS\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);

    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      ".main STA v : JMP (vector) : CANJUMP hA\n"
                      ".hA LDA v : RTS\n"
                      ".vector EQUW hA\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);
}

RC_TEST_STEP(assemble, zpinterrupt_pins_comm_var, fix)
{
    // `flag` is written by the mainline and read by the handler - live-in at the handler entry, so it is
    // pinned against EVERYTHING: the mainline may store to it at any instant relative to the handler, so
    // no byte reuse exists for it, the handler's own temp included.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 flag, t, ht\n"
                      ".main STA flag : STA t : LDA t : RTS\n"
                      ".irq ZPINTERRUPT : LDA flag : STA ht : LDA ht : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    int64_t flag = zp_addr(&fix->r, "flag");
    int64_t t    = zp_addr(&fix->r, "t");
    int64_t ht   = zp_addr(&fix->r, "ht");
    RC_CHECK_TRUE(flag >= 0 && t >= 0 && ht >= 0);
    RC_CHECK_TRUE(flag != t);
    RC_CHECK_TRUE(flag != ht);
    RC_CHECK_TRUE(t != ht);   // footprint isolation separates the handler temp from the mainline temp too
}

RC_TEST_STEP(assemble, zpentry_input_warns, fix)
{
    // A ZPENTRY routine reading `v` before writing it expects its caller to have poked the value - which
    // an outside caller cannot do at an allocator-chosen address. Warn, name the variable, allocate anyway.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      ".main ZPENTRY : LDA v : STA v : RTS\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 1u);
    RC_CHECK(diag_payload(&fix->r, error_type_zpentry_input), ==, RC_STR("v"));
    RC_CHECK_TRUE(zp_addr(&fix->r, "v") >= 0);

    // Written before read is an ordinary temp - nothing to say.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      ".main ZPENTRY : STA v : LDA v : RTS\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 0u);

    // The read may sit a call deep in the routine's extent - the footprint walk still sees it.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      ".main ZPENTRY : JSR sub : RTS\n"
                      ".sub LDA v : STA v : RTS\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 1u);
}

RC_TEST_STEP(assemble, zpentry_input_ignores_threaded_liveness, fix)
{
    // `keep` is held live ACROSS an in-program call to the marked routine, so the return edges thread it
    // through and it shows up live-in at the entry - but the routine never reads it unwritten, so it is
    // not an input. The read-before-write walk is exactly what keeps this quiet.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 keep, w\n"
                      ".main ZPENTRY : STA keep : JSR rout : LDA keep : RTS\n"
                      ".rout ZPENTRY : STA w : LDA w : RTS\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 0u);
}

RC_TEST_STEP(assemble, zpentry_input_ignores_shared_helper_smear, fix)
{
    // The demo shape that broke the first cut of this warning: a helper called both from the entry's
    // pre-init stretch and from inside the main loop. The loop call site's live-after (f, live around
    // the loop) smears through the helper's shared return edge into the entry's unrelated call site,
    // so plain live-in claims f is an input - but f is written before every real read from the entry.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 f\n"
                      ".mainloop LDA f : JSR shared : JMP mainloop\n"
                      ".shared LDX #0 : RTS\n"
                      ".entry ZPENTRY : JSR shared : LDA #0 : STA f : JMP mainloop\n"
                      ".irq ZPINTERRUPT : INC f : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 0u);

    // Contrast: a genuinely conditional init IS an input - the untaken path reaches the read unwritten.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 f\n"
                      ".entry ZPENTRY : BEQ over : STA f : .over LDA f : STA f : RTS\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 1u);
    RC_CHECK(diag_payload(&fix->r, error_type_zpentry_input), ==, RC_STR("f"));
}

RC_TEST_STEP(assemble, zpentry_input_is_byte_accurate, fix)
{
    // Seeding only a pointer's low byte leaves the high byte an input to the deref...
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr\n"
                      ".main ZPENTRY : LDA #0 : STA ptr : TAY : LDA (ptr),Y : STA ptr+1 : RTS\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 1u);

    // ...while seeding both bytes before the deref is a fully-initialised temp.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO2 ptr\n"
                      ".main ZPENTRY : LDA #0 : STA ptr : STA ptr+1 : TAY : LDA (ptr),Y : STA ptr : RTS\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 0u);
}

RC_TEST_STEP(assemble, zpentry_input_spares_handlers, fix)
{
    // A handler's live-in comm var IS the supported pattern (Guard 3 pins it) - no input warning there...
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 flag\n"
                      ".main ZPENTRY : STA flag : RTS\n"
                      ".irq ZPINTERRUPT : LDA flag : RTI\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 0u);

    // ...and a stacked ZPENTRY+ZPINTERRUPT on one pc takes the stricter handler treatment, so the sync
    // marker stays quiet too.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 flag\n"
                      ".main ZPENTRY : STA flag : LDA flag : RTS\n"
                      ".irq ZPENTRY : ZPINTERRUPT : LDA flag : RTI\n") != 0);
    RC_CHECK(diag_count(&fix->r, error_type_zpentry_input), ==, 0u);
}

RC_TEST_STEP(assemble, zpinterrupt_separates_temps_from_mainline, fix)
{
    // The control: without the marker the handler is an island, its temp's range overlaps nothing the
    // analysis can see, and first-fit happily packs both temps onto one byte - the silent clobber.
    #define IRQ_TEMPS(marker) \
        "ZPRESERVE &70..&7F : ZPAUTO1 m, h\n" \
        ".main STA m : LDA m : RTS\n" \
        ".irq " marker "STA h : LDA h : RTI\n"
    RC_CHECK_TRUE(ASM(IRQ_TEMPS("")) != 0);
    RC_CHECK(zp_addr(&fix->r, "m"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "h"), ==, 0x70);

    // With it, the handler's footprint interferes with everything outside - the temps split.
    RC_CHECK_TRUE(ASM(IRQ_TEMPS("ZPINTERRUPT : ")) != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(zp_addr(&fix->r, "m") != zp_addr(&fix->r, "h"));
    #undef IRQ_TEMPS
}

RC_TEST_STEP(assemble, zpinterrupt_handler_temps_still_share, fix)
{
    // Inside the handler, ordinary liveness still governs: two temps with disjoint ranges share a byte.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h1, h2\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq ZPINTERRUPT : STA h1 : LDA h1 : STA h2 : LDA h2 : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "h1"), ==, zp_addr(&fix->r, "h2"));
    RC_CHECK_TRUE(zp_addr(&fix->r, "m") != zp_addr(&fix->r, "h1"));
}

RC_TEST_STEP(assemble, zpinterrupt_two_handlers_separated, fix)
{
    // An NMI can preempt an IRQ handler mid-flight, so two handlers' footprints must not share either -
    // which falls out of each footprint interfering with everything outside itself.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, hi, hn\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq ZPINTERRUPT : STA hi : LDA hi : RTI\n"
                      ".nmi ZPINTERRUPT : STA hn : LDA hn : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(zp_addr(&fix->r, "hi") != zp_addr(&fix->r, "hn"));
    RC_CHECK_TRUE(zp_addr(&fix->r, "m") != zp_addr(&fix->r, "hi"));
    RC_CHECK_TRUE(zp_addr(&fix->r, "m") != zp_addr(&fix->r, "hn"));
}

RC_TEST_STEP(assemble, zpinterrupt_unknown_call_refused, fix)
{
    // A handler whose extent reaches computed flow has an unboundable footprint: the pinning cannot be
    // applied soundly, so the marker refuses (alongside Guard 1's own complaint at the jump itself). The
    // remedy is the same as ever - declare the targets.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq ZPINTERRUPT : STA h : LDA h : JMP (vector)\n"
                      ".vector EQUW irq\n") == 0u);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_across_call));
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_computed_flow));

    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h\n"
                      ".main STA m : LDA m : RTS\n"
                      ".irq ZPINTERRUPT : STA h : LDA h : JMP (vector) : CANJUMP done\n"
                      ".done RTI\n"
                      ".vector EQUW done\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, zpentry_zero_vars_is_noop, fix)
{
    // No ZPAUTO variables: nothing to analyse, and a well-placed marker is a clean no-op...
    RC_CHECK_TRUE(ASM("ZPRESERVE &70 : .main ZPENTRY : LDA #1 : RTS") != 0);
    RC_CHECK(fix->r.diagnostics.num, ==, 0u);
    // ...but a marker sitting on nothing is still a static mistake worth refusing.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPENTRY : EQUB 1") == error_type_zpentry_no_code);
}

RC_TEST_STEP(assemble, zpentry_duplicates_and_dual_decl, fix)
{
    // Stacked markers on one pc collapse: one root, one handler record - and the stricter (interrupt)
    // treatment applies. No duplicate diagnostics from the repetition.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      ".main ZPENTRY : ZPENTRY : ZPINTERRUPT : STA v : LDA v : RTS\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
}

RC_TEST_STEP(assemble, zpinterrupt_unused_var_is_inert, fix)
{
    // Pinning may aim edges at an unused variable, but unused is decided first and the colourer skips it:
    // still just the unused warning, no address, no spill.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&71 : ZPAUTO1 used, gap\n"
                      ".main STA used : LDA used : RTS\n"
                      ".irq ZPINTERRUPT : LDA used : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_unused));
    RC_CHECK(zp_addr(&fix->r, "used"), ==, 0x70);
    RC_CHECK_TRUE(value_is_none(baron_result_symbol(&fix->r, RC_STR("gap"))));
}

RC_TEST_STEP(assemble, zpinterrupt_handler_also_called, fix)
{
    // A handler can be installed in a vector AND called directly (a shared service routine): the call
    // machinery and the pinning are independent and compose. Reached both ways, so no warning either.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 m, h\n"
                      ".main STA m : JSR irq : LDA m : RTS\n"
                      ".irq ZPINTERRUPT : STA h : LDA h : RTI\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(diag_count(&fix->r, error_type_zpauto_unreachable), ==, 0u);
    RC_CHECK_TRUE(zp_addr(&fix->r, "m") != zp_addr(&fix->r, "h"));
}

RC_TEST_STEP(assemble, discard_confines_indexed_array, fix)
{
    // The spritescale shape in miniature: two JMP-dispatched alternates on a loop. `arr` is initialised
    // only through `STA arr,X` - no provable byte written - so its reads leak liveness back to the
    // routine entry and around the loop, through the sibling: without DISCARD, `t` must dodge all of it.
    #define ALTERNATES(marker) \
        "ZPRESERVE &70..&7F\n" \
        ".loop LDA &90 : BEQ done : LSR A : BCC toa\n" \
        "JMP rb\n" \
        ".toa JMP ra\n" \
        ".done RTS\n" \
        ".ra { ZPAUTO 4, arr : " marker "LDX #0\n" \
        ".l STA arr,X : INX : CPX #4 : BNE l\n" \
        "LDA arr+0 : STA &91 : JMP loop }\n" \
        ".rb { ZPAUTO1 t : STA t : LDA t : STA &91 : JMP loop }\n"
    RC_CHECK_TRUE(ASM(ALTERNATES("")) != 0);
    RC_CHECK(zp_addr(&fix->r, "ra.arr"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "rb.t"), ==, 0x74);     // arr leaks through rb, so t dodges its span

    RC_CHECK_TRUE(ASM(ALTERNATES("DISCARD arr : ")) != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "ra.arr"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "rb.t"), ==, 0x70);     // the promise confines arr; t reuses its first byte
    #undef ALTERNATES
}

RC_TEST_STEP(assemble, discard_operand_errors, fix)
{
    // Only a whole ZPAUTO variable can be discarded: a number, a var+n slice, or a label is refused, an
    // unknown name defers and errors on the final pass, and the keyword itself is reserved.
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : DISCARD 5") == error_type_discard_needs_var);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : ZPAUTO1 v : STA v : LDA v : DISCARD v+1") == error_type_discard_needs_var);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : .lbl DISCARD lbl") == error_type_discard_needs_var);
    RC_CHECK_TRUE(ERR("ZPRESERVE &70 : DISCARD nothere") == error_type_undefined_symbol);
    RC_CHECK_TRUE(ERR(".discard RTS") == error_type_expected_label_name);

    // The happy path parses as a comma list, like the other annotations.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 u, w : STA u : LDA u : STA w : LDA w\n"
                      "DISCARD u, w : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
}

RC_TEST_STEP(assemble, discard_only_var_is_unused, fix)
{
    // A DISCARD is a promise about a value, not a use of one: a variable nothing else touches is still
    // unused - warned, unplaced, undefined - and the stray marker upsets nothing.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 used, ghost\n"
                      ".main STA used : LDA used : DISCARD ghost : RTS") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_zpauto_unused));
    RC_CHECK(zp_addr(&fix->r, "used"), ==, 0x70);
    RC_CHECK_TRUE(value_is_none(baron_result_symbol(&fix->r, RC_STR("ghost"))));
}

RC_TEST_STEP(assemble, discard_keeps_annotation_site_binding, fix)
{
    // CANCALL/CANJUMP bind to the previous INSTRUCTION; a DISCARD in between is a marker, not a site, so
    // the RTS-dispatch annotation still lands on the RTS - the target stays wired (and so reachable).
    RC_CHECK_TRUE(ASM("ZPRESERVE &70..&7F : ZPAUTO1 v\n"
                      "LDA #0 : PHA : PHA\n"
                      "RTS : DISCARD v : CANJUMP target\n"
                      ".target STA v : LDA v : RTS\n") != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_zpauto_unreachable));
    RC_CHECK(zp_addr(&fix->r, "v"), ==, 0x70);
}

RC_TEST_STEP(assemble, discard_is_must_write_at_the_call, fix)
{
    // Inside a JSR-called routine the promise counts as a definite rewrite: the caller's pre-call value
    // dies at the call, so `b1` - alive only between prep's store and the JSR - can share prep's byte.
    #define PREP_CALL(body) \
        "ZPRESERVE &70..&7F : ZPAUTO1 prep, b1\n" \
        ".main STA prep : STA b1 : LDA b1 : JSR sub : LDA prep : RTS\n" \
        ".sub " body " RTS\n"
    RC_CHECK_TRUE(ASM(PREP_CALL("STA &90 :")) != 0);
    RC_CHECK(zp_addr(&fix->r, "b1"), ==, 0x71);       // prep is live across the call: no sharing

    RC_CHECK_TRUE(ASM(PREP_CALL("DISCARD prep :")) != 0);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_none);
    RC_CHECK(zp_addr(&fix->r, "prep"), ==, 0x70);
    RC_CHECK(zp_addr(&fix->r, "b1"), ==, 0x70);       // the pre-call value died at the JSR
    #undef PREP_CALL
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

RC_TEST_STEP(assemble, result_exposes_sources, fix)
{
    // The result carries every source the assemble touched (root first, INCLUDEs after), so a diagnostic's
    // cursor - a source index + byte offset - is resolvable to a name and text by the caller.
    uint32_t passes = (fix->r = assemble_string(&fix->desc, RC_STR("top"), RC_STR("include \"inc_child.6502\"")),
                       fix->r.passes);
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK(fix->r.sources.num, ==, 2u);
    RC_CHECK(rc_view_source_file_get(fix->r.sources, 0).name, ==, RC_STR("top"));
    RC_CHECK(rc_view_source_file_get(fix->r.sources, 1).name, ==, RC_STR("inc_child.6502"));
}

RC_TEST_STEP(assemble, section_copies_survive_next_assemble, fix)
{
    // The contract the CLI stands on: a result's sections die at the next assemble on the same arenas
    // (per_pass reset), but section_make_copy into a caller arena keeps them. Assemble A, copy, assemble B,
    // and A's copied bytes must be untouched by B's pass machinery.
    rc_arena kept = rc_arena_make_default();
    RC_CHECK_TRUE(ASM("LDA #&12 : RTS") != 0);
    section copy = section_make_copy(rc_view_section_get(fix->r.sections, 0), &kept);

    RC_CHECK_TRUE(ASM("LDX #&EE : LDY #&FF : NOP") != 0);   // supersedes the first result's sections

    RC_CHECK(copy.code.num, ==, 3u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 0), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 1), ==, 0x12u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 2), ==, 0x60u);
    rc_arena_deinit(&kept);
}

RC_TEST_STEP(assemble, listing_is_opt_in, fix)
{
    // Without baron_desc.verbose the listing pass never runs: the result's listing is empty and the
    // sections are the settling pass's (patched by the allocator - the same bytes either way).
    RC_CHECK_TRUE(ASM("lda #1 : rts") != 0);
    RC_CHECK(VERB().len, ==, 0u);
    RC_CHECK_TRUE(code_is(&fix->r, fix->r.passes, (uint8_t[]){0xA9, 0x01, 0x60}, 3));
}

RC_TEST_STEP(assemble, listing_full_shape, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // The whole listing format in one program: section framing at the margin (blank line after the
    // close), labels at the margin, instructions and data with address + hex + verbatim source, EQUS
    // truncated after four bytes, assignments and ZPAUTO allocations echoed at the margin -
    // and the ZPAUTO crown jewels: the declaration lists as the assignment it became (`var = &70 [auto]`)
    // and `sta var` shows the ALLOCATED byte (&70), not the placeholder, because the listing pass runs
    // after allocation has rewritten the symbols.
    RC_CHECK_TRUE(ASM("section main, org=&900\nzpreserve &70..&7F\nzpauto1 var\n.label\nlda #&12\n"
                      "sta var\n.inner\nldx #1\nlda var\nrts\nequs \"ABCDEFGH\"\nequb 0\nendsection\nx = 5") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("section main, org=&900\n"
                    "var = &70 [auto]\n"
                    ".label\n"
                    "  0900  A9 12           lda #&12\n"
                    "  0902  85 70           sta var\n"
                    ".inner\n"
                    "  0904  A2 01           ldx #1\n"
                    "  0906  A5 70           lda var\n"
                    "  0908  60              rts\n"
                    "  0909  41 42 43 44...  equs \"ABCDEFGH\"\n"
                    "  0911  00              equb 0\n"
                    "endsection\n"
                    "\n"
                    "x = 5\n"));
}

RC_TEST_STEP(assemble, listing_macro_expansion, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // The invocation line carries the address and an empty byte field; the expansion's lines follow with
    // the bytes, echoing the BODY's source (parameter names and all).
    RC_CHECK_TRUE(ASM("macro add8 addr\nclc\nlda addr\nendmacro\nadd8 &70") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("  0000                  add8 &70\n"
                    "  0000  18              clc\n"
                    "  0001  A5 70           lda addr\n"));
}

RC_TEST_STEP(assemble, listing_include_child, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // The INCLUDE line, then the child's lines echoed from the CHILD's source (which is also the left-trim
    // case: inc_child.6502 is `.child ldx #2` on one line, so the ldx statement's span leads with a space).
    // Named explicitly ("top") because ASM's name-is-the-text would grow a bogus directory from the slashes.
    fix->r = assemble_string(&fix->desc, RC_STR("top"), RC_STR("include \"inc_child.6502\""));
    RC_CHECK_TRUE(fix->r.passes != 0);
    RC_CHECK(fix->r.channels[0], ==,
             RC_STR("  0000                  include \"inc_child.6502\"\n"
                    ".child\n"
                    "  0000  A2 02           ldx #2\n"));
}

RC_TEST_STEP(assemble, listing_for_repeats, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // A FOR body lists once per iteration - that is what actually assembled.
    RC_CHECK_TRUE(ASM("for i = 1..3\nequb i\nnext") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("  0000  01              equb i\n"
                    "  0001  02              equb i\n"
                    "  0002  03              equb i\n"));
}

RC_TEST_STEP(assemble, listing_dead_branch_and_failure, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // A dead branch leaves no trace (the gate is listing && active), and a failed assemble has no listing
    // at all.
    RC_CHECK_TRUE(ASM("if false\nlda #1\nendif\nrts") != 0);
    RC_CHECK(VERB(), ==, RC_STR("  0000  60              rts\n"));

    RC_CHECK(ASM("lda"), ==, 0u);
    RC_CHECK(VERB().len, ==, 0u);
}

RC_TEST_STEP(assemble, listing_local_label, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    RC_CHECK_TRUE(ASM("ldx #2\n.@\ndex\nbne @-") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("  0000  A2 02           ldx #2\n"
                    ".@\n"
                    "  0002  CA              dex\n"
                    "  0003  D0 FD           bne @-\n"));
}

RC_TEST_STEP(assemble, listing_skip_and_multiline, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // SKIP's zero padding truncates like any long dump; a multi-line list literal echoes only its first
    // line, closed with an ellipsis (the BYTES are all there - only the source echo is cut).
    RC_CHECK_TRUE(ASM("skip 8") != 0);
    RC_CHECK(VERB(), ==, RC_STR("  0000  00 00 00 00...  skip 8\n"));

    RC_CHECK_TRUE(ASM("equb {1,\n2}") != 0);
    RC_CHECK(VERB(), ==, RC_STR("  0000  01 02           equb {1,...\n"));
}

RC_TEST_STEP(assemble, listing_assignments_and_braces, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // Assignments echo verbatim at the margin (they emit nothing and land nowhere); braces echo at the
    // margin too, so the scope structure survives into the listing, whether the '{' shares the label's
    // line or not; and a ZPAUTO declaration lists as the assignment it became: `tmp = &70 [auto]`.
    RC_CHECK_TRUE(ASM("zpreserve &70..&7F\nbase = &12\n.sub {\nzpauto1 tmp\nsta tmp\nlda #base\nrts\n}") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("base = &12\n"
                    ".sub\n"
                    "{\n"
                    "tmp = &70 [auto]\n"
                    "  0000  85 70           sta tmp\n"
                    "  0002  A9 12           lda #base\n"
                    "  0004  60              rts\n"
                    "}\n"));

    // An anonymous scope's braces list the same way, and a dead branch's do not list at all (the gate is
    // listing && active, same as every other line).
    RC_CHECK_TRUE(ASM("{\nnop\n}\nif false\n{\nw = 1\n}\nendif") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("{\n"
                    "  0000  EA              nop\n"
                    "}\n"));
}

RC_TEST_STEP(assemble, error_statement, fix)
{
    // ERROR records the user's message as a RECOVERABLE error: the assemble fails, but parsing carries on
    // so later errors still accumulate. The message is PRINT-formatted (strings raw, numbers in
    // value_format's shape, concatenated) and rides in the diagnostic's payload.
    RC_CHECK(ASM("LDA #1\nERROR \"bad config: \", 42\nLDA nosuch"), ==, 0u);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_user_error);
    RC_CHECK(diag_payload(&fix->r, error_type_user_error), ==, RC_STR("bad config: 42"));
    RC_CHECK_TRUE(has_diag(&fix->r, error_type_undefined_symbol));   // the statement after still parsed

    // A forward reference in the message defers like PRINT's and formats once settled.
    RC_CHECK(ASM("ERROR \"at \", target\nSKIP 5\n.target"), ==, 0u);
    RC_CHECK(diag_payload(&fix->r, error_type_user_error), ==, RC_STR("at 5"));

    // A dead branch records nothing; a bare ERROR fires with an empty message.
    RC_CHECK_TRUE(ASM("IF FALSE\nERROR \"no\"\nENDIF\nRTS") != 0);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_user_error));
    RC_CHECK(ASM("ERROR"), ==, 0u);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_user_error);
    RC_CHECK(diag_payload(&fix->r, error_type_user_error).len, ==, 0u);

    // A message symbol still unknown on the final pass is the usual undefined-symbol error (naming the
    // symbol); the ERROR itself stays silent, since its message could never be formed.
    RC_CHECK_TRUE(ERR("ERROR nosuch") == error_type_undefined_symbol);
    RC_CHECK_FALSE(has_diag(&fix->r, error_type_user_error));
    RC_CHECK(diag_payload(&fix->r, error_type_undefined_symbol), ==, RC_STR("nosuch"));
}

RC_TEST_STEP(assemble, diagnostics_carry_payloads, fix)
{
    // The payload plumbing end to end: an undefined symbol names itself (through the evaluator's error
    // detail), a duplicate names the symbol on both the error and its companion note, and a branch out of
    // range reports the distance it would need.
    RC_CHECK_TRUE(ERR("LDA zork+1") == error_type_undefined_symbol);
    RC_CHECK(diag_payload(&fix->r, error_type_undefined_symbol), ==, RC_STR("zork"));

    RC_CHECK_TRUE(ERR(".here\n.here") == error_type_duplicate_symbol);
    RC_CHECK(diag_payload(&fix->r, error_type_duplicate_symbol), ==, RC_STR("here"));
    RC_CHECK(diag_payload(&fix->r, error_type_original_definition), ==, RC_STR("here"));

    // BEQ at pc 0: the delta is measured from the address after the operand byte (2), so the target at
    // 202 is +200 - and the payload says so. A backward branch reports a negative distance.
    RC_CHECK_TRUE(ERR("BEQ far : SKIP 200 : .far RTS") == error_type_branch_out_of_range);
    RC_CHECK(diag_payload(&fix->r, error_type_branch_out_of_range), ==, RC_STR("+200"));
    RC_CHECK_TRUE(ERR(".back : SKIP 200 : BNE back") == error_type_branch_out_of_range);
    RC_CHECK(diag_payload(&fix->r, error_type_branch_out_of_range), ==, RC_STR("-202"));

    // The unused-ZPAUTO warning names the variable.
    RC_CHECK_TRUE(ASM("ZPRESERVE &70 : ZPAUTO1 spare : RTS") != 0);
    RC_CHECK(diag_payload(&fix->r, error_type_zpauto_unused), ==, RC_STR("spare"));
}

RC_TEST_STEP(assemble, define_binds_symbol, fix)
{
    // The CLI's -D switch: each desc.defines entry is one "name=expression" bound into the root scope
    // before the source parses, so the very first statement can already read it. The expression gets
    // the full evaluator - numbers, strings, built-in constants - and a later define may read an
    // earlier one (they apply in command-line order).
    static const rc_str defs[] = {RC_STR_INIT("screenwidth=64"), RC_STR_INIT("debug=TRUE"),
                                  RC_STR_INIT("version=\"1.0\""), RC_STR_INIT("half=screenwidth/2")};
    fix->desc.defines = (rc_view_str) RC_VIEW(defs);
    RC_CHECK_TRUE(code_is(&fix->r, ASM("LDA #screenwidth : EQUB half"), (uint8_t[]) {0xA9, 0x40, 0x20}, 3));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("debug")), value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("version")), value_make_string(RC_STR("1.0"))));
}

RC_TEST_STEP(assemble, define_forward_reference, fix)
{
    // A -D expression may reference symbols the SOURCE defines - unresolved on the first pass,
    // settled once the label binds, exactly like any forward reference. The defines may lean on
    // each other in either direction too: an earlier one naming a later one just takes a pass.
    static const rc_str defs[] = {RC_STR_INIT("total=limit*2"), RC_STR_INIT("first=second+1"),
                                  RC_STR_INIT("second=10")};
    fix->desc.defines = (rc_view_str) RC_VIEW(defs);
    RC_CHECK_TRUE(ASM("SKIP 5\n.limit") != 0);
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("total")), value_make_numeric(10)));
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("first")), value_make_numeric(11)));
}

RC_TEST_STEP(assemble, define_duplicate_and_default_idiom, fix)
{
    // A source assignment to a -D name is a duplicate - the predefinition came first and stands -
    // and the companion note points into the -D's own synthetic source, so the report names the switch.
    static const rc_str defs[] = {RC_STR_INIT("debug=1")};
    fix->desc.defines = (rc_view_str) RC_VIEW(defs);
    RC_CHECK_TRUE(ERR("debug = 0") == error_type_duplicate_symbol);
    bool named = false;
    for (uint32_t i = 0; i < fix->r.diagnostics.num; i++) {
        diagnostic d = rc_view_diagnostic_get(fix->r.diagnostics, i);
        if (d.code == error_type_original_definition && d.at.source < fix->r.sources.num) {
            named = rc_str_is_equal(rc_view_source_file_get(fix->r.sources, d.at.source).name,
                                    RC_STR("-D debug=1"));
        }
    }
    RC_CHECK_TRUE(named);

    // So a source default guards with DEFINED and binds a DIFFERENT name (self-guarding -
    // IF defined(x) == FALSE : x = 0 - cannot converge: binding x flips its own condition, and the
    // then-dead branch removes the binding again). The aliased form settles both ways.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF defined(debug)\ndbg = debug\nELSE\ndbg = 0\nENDIF\nEQUB dbg"),
                          (uint8_t[]) {0x01}, 1));
    fix->desc.defines = (rc_view_str) {0};
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF defined(debug)\ndbg = debug\nELSE\ndbg = 0\nENDIF\nEQUB dbg"),
                          (uint8_t[]) {0x00}, 1));

    // Two -D of the same name collide the same way (their strings differ, so their identities do).
    static const rc_str twice[] = {RC_STR_INIT("x=1"), RC_STR_INIT("x=2")};
    fix->desc.defines = (rc_view_str) RC_VIEW(twice);
    RC_CHECK_TRUE(ERR("RTS") == error_type_duplicate_symbol);
}

RC_TEST_STEP(assemble, define_undefined_and_value_errors, fix)
{
    // A define's symbol still unknown on the final pass is the usual undefined-symbol error, naming
    // the symbol; a value error (x=1/0) surfaces on the final pass like any assignment's would.
    static const rc_str unknown[] = {RC_STR_INIT("x=nothing")};
    fix->desc.defines = (rc_view_str) RC_VIEW(unknown);
    RC_CHECK_TRUE(ERR("RTS") == error_type_undefined_symbol);
    RC_CHECK(diag_payload(&fix->r, error_type_undefined_symbol), ==, RC_STR("nothing"));

    static const rc_str div0[] = {RC_STR_INIT("x=1/0")};
    fix->desc.defines = (rc_view_str) RC_VIEW(div0);
    RC_CHECK_TRUE(ERR("RTS") == error_type_divide_by_zero);
}

RC_TEST_STEP(assemble, define_malformed, fix)
{
    // A definition that can never come right is fatal on the first pass: a reserved name (the same
    // collision rules the source has), a missing '=', text left over after the expression, a dotted name.
    static const rc_str no_eq[]    = {RC_STR_INIT("just_a_name")};
    static const rc_str trailing[] = {RC_STR_INIT("x=1:y=2")};
    static const rc_str reserved[] = {RC_STR_INIT("pi=5")};
    static const rc_str mnemonic[] = {RC_STR_INIT("lda=1")};
    static const rc_str dotted[]   = {RC_STR_INIT("a.b=1")};

    fix->desc.defines = (rc_view_str) RC_VIEW(no_eq);
    RC_CHECK_TRUE(ERR("RTS") == error_type_expected_assign);
    fix->desc.defines = (rc_view_str) RC_VIEW(trailing);
    RC_CHECK_TRUE(ERR("RTS") == error_type_expected_end_of_expression);
    fix->desc.defines = (rc_view_str) RC_VIEW(reserved);
    RC_CHECK_TRUE(ERR("RTS") == error_type_expected_var_name);
    fix->desc.defines = (rc_view_str) RC_VIEW(mnemonic);
    RC_CHECK_TRUE(ERR("RTS") == error_type_expected_var_name);
    fix->desc.defines = (rc_view_str) RC_VIEW(dotted);
    RC_CHECK_TRUE(ERR("RTS") == error_type_invalid_assignment);
}

RC_TEST_STEP(assemble, define_listing, fix)
{
    fix->desc.verbose = true;   // the listing is opt-in
    // A define echoes at the margin like the assignment it is, ahead of the source's first line.
    static const rc_str defs[] = {RC_STR_INIT("screenwidth=64")};
    fix->desc.defines = (rc_view_str) RC_VIEW(defs);
    RC_CHECK_TRUE(ASM("lda #screenwidth") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("screenwidth=64\n"
                    "  0000  A9 40           lda #screenwidth\n"));
}

RC_TEST_STEP(assemble, print_to_channel_zero, fix)
{
    // Without -v, PRINT writes its channel on the final pass - so each statement speaks exactly once,
    // multi-pass or not. Values concatenate with NO separator (spacing belongs to the writer): strings
    // raw, everything else in value_format's shape. A bare PRINT is a blank line.
    RC_CHECK_TRUE(ASM("print \"x = \", 42\nprint\nprint 1, \" and \", {2, 3}") != 0);
    RC_CHECK(fix->r.channels[0], ==, RC_STR("x = 42\n\n1 and {2, 3}\n"));
    RC_CHECK(fix->r.channels[1].len, ==, 0u);
}

RC_TEST_STEP(assemble, print_channels_and_syntax, fix)
{
    // #n, routes to channel n; #0 is just the default spelled out.
    RC_CHECK_TRUE(ASM("print #1, \"debug\"\nprint #0, \"main\"") != 0);
    RC_CHECK(fix->r.channels[0], ==, RC_STR("main\n"));
    RC_CHECK(fix->r.channels[1], ==, RC_STR("debug\n"));

    // A malformed channel is fatal: two digits, or a missing comma.
    RC_CHECK(ASM("print #12, 1"), ==, 0u);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_bad_print_channel);
    RC_CHECK(ASM("print #1 1"), ==, 0u);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_bad_print_channel);
}

RC_TEST_STEP(assemble, print_interleaves_listing, fix)
{
    fix->desc.verbose = true;   // now PRINT speaks on the LISTING pass, into the listing's own channel 0
    RC_CHECK_TRUE(ASM("lda #1\nprint \"pc is \", *\nrts") != 0);
    RC_CHECK(VERB(), ==,
             RC_STR("  0000  A9 01           lda #1\n"
                    "pc is 2\n"
                    "  0002  60              rts\n"));
}

RC_TEST_STEP(assemble, print_forward_reference_and_for, fix)
{
    // A forward reference defers like any other and prints its settled value, once.
    RC_CHECK_TRUE(ASM("print target\nskip 5\n.target") != 0);
    RC_CHECK(fix->r.channels[0], ==, RC_STR("5\n"));

    // A FOR body prints per iteration.
    RC_CHECK_TRUE(ASM("for i = 1..3\nprint i\nnext") != 0);
    RC_CHECK(fix->r.channels[0], ==, RC_STR("1\n2\n3\n"));
}

RC_TEST_STEP(assemble, print_dead_branch_and_undefined, fix)
{
    // A dead branch parses its PRINT but says nothing.
    RC_CHECK_TRUE(ASM("if false\nprint \"no\"\nendif\nprint \"yes\"") != 0);
    RC_CHECK(fix->r.channels[0], ==, RC_STR("yes\n"));

    // An operand still unknown on the final pass is the usual undefined symbol, and a failed assemble
    // hands back empty channels - same contract as the listing.
    RC_CHECK(ASM("print nosuch"), ==, 0u);
    RC_CHECK_TRUE(first_error(&fix->r) == error_type_undefined_symbol);
    RC_CHECK(fix->r.channels[0].len, ==, 0u);
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
    uint32_t passes = (fix->r = assemble_file(&fix->desc, RC_STR("sample.6502"))).passes;
    RC_CHECK_TRUE(code_is(&fix->r, passes, (uint8_t[]){0xA9, 0x01, 0x60}, 3));

    RC_CHECK_TRUE((fix->r = assemble_file(&fix->desc, RC_STR("no_such_file.6502")),
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

RC_TEST_STEP(assemble, boolean_values_coerce, fix)
{
    // A boolean coerces to 1 / 0 wherever a number is wanted: data bytes, arithmetic, conditions.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB TRUE, FALSE, 2>1"), (uint8_t[]){0x01, 0x00, 0x01}, 3));
    // The comparison-mask idiom: comparisons yield booleans, and multiplying them coerces.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB (2>1)*(3>2)*5"), (uint8_t[]){0x05}, 1));
    // A condition is a boolean or a number's zero/nonzero truthiness - both live here.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("IF TRUE : EQUB 1 : ENDIF : IF 5 : EQUB 2 : ENDIF"),
                          (uint8_t[]){0x01, 0x02}, 2));
    // PRINT renders a boolean by name.
    RC_CHECK_TRUE(ASM("PRINT TRUE, \" \", FALSE, \" \", 1=1") != 0);
    RC_CHECK(fix->r.channels[0], ==, RC_STR("TRUE FALSE TRUE\n"));
    // A mixed boolean/number pair under AND/OR/EOR is refused, and the refusal surfaces here.
    RC_CHECK_TRUE(ERR("EQUB TRUE and 1") == error_type_type_mismatch);
}

RC_TEST_STEP(assemble, beebasm_true_mode, fix)
{
    // --beebasm-true: TRUE coerces to -1, so it emits as &FF and equals -1 rather than 1.
    value_set_beebasm_true(true);
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB TRUE, FALSE, -1 = TRUE"), (uint8_t[]){0xFF, 0x00, 0xFF}, 3));
    value_set_beebasm_true(false);
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
#define INC(src) (fix->r = assemble_string(&fix->desc, RC_STR("top"), RC_STR(src)), fix->r.passes)

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
    RC_CHECK((fix->r = assemble_string(&fix->desc, RC_STR("top2"), RC_STR("include missing_name"))).passes, ==, 0u);
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
    uint32_t passes = (fix->r = assemble_file(&fix->desc, RC_STR("inc_cycle.6502"))).passes;
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

    uint32_t passes = (fix->r = assemble_string(&fix->desc, RC_STR("t"),
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

RC_TEST_STEP(assemble, mapchar_idioms, fix)
{
    // The MAPCHAR replacement, NumPy style: codes() bridges text to numbers, find() maps each
    // code to its index in a charset string, and a gather subscript is the lookup table.
    RC_CHECK_TRUE(code_is(&fix->r, ASM(
        "glyphs = \"ABC \"\n"
        "FUNCTION mapchar(s) = find(codes(glyphs), codes(s))\n"
        "EQUB mapchar(\"CAB BA\")"), (uint8_t[]) {2, 0, 1, 3, 1, 0}, 6));

    // The {old, new} pair-list flavour: search one list, subscript the other with the indices.
    RC_CHECK_TRUE(code_is(&fix->r, ASM(
        "from = codes(\"XYZ\")\n"
        "to   = {7, 8, 9}\n"
        "FUNCTION remap(s) = to[find(from, codes(s))]\n"
        "EQUB remap(\"ZYX\")"), (uint8_t[]) {9, 8, 7}, 3));

    // The offset flavour needs no FUNCTION at all: a length-1 codes() list broadcasts.
    RC_CHECK_TRUE(code_is(&fix->r, ASM("EQUB codes(\"HAL\") - codes(\" \")"), (uint8_t[]) {40, 33, 44}, 3));

    // A character missing from the charset fails the assemble loudly, naming the code.
    RC_CHECK_TRUE(ERR(
        "glyphs = \"ABC\"\n"
        "FUNCTION mapchar(s) = find(codes(glyphs), codes(s))\n"
        "EQUB mapchar(\"AQ\")") == error_type_not_found);
    RC_CHECK(diag_payload(&fix->r, error_type_not_found), ==, RC_STR("81"));   // 'Q'
}

RC_TEST_STEP(assemble, function_error_value, fix)
{
    // error() in a body: the guard branch fires and its message lands as a user_error at the
    // use site; the happy path never sees it (a dead branch's error() is built, then dropped).
    #define CHECKED_FN \
        "FUNCTION checked(w)\n" \
        "IF w < 0\n" \
        "r = error(\"bad width: \", w)\n" \
        "ELSE\n" \
        "r = w\n" \
        "ENDIF\n" \
        "= r\n"
    RC_CHECK_TRUE(code_is(&fix->r, ASM(CHECKED_FN "EQUB checked(5)"), (uint8_t[]) {5}, 1));
    RC_CHECK_TRUE(ERR(CHECKED_FN "EQUB checked(0-3)") == error_type_user_error);
    RC_CHECK(diag_payload(&fix->r, error_type_user_error), ==, RC_STR("bad width: -3"));
    #undef CHECKED_FN
}

RC_TEST_STEP(assemble, function_duplicate_local, fix)
{
    // Body locals are single-assignment: a second live assignment to one name used to be
    // silently ignored (reading like mutation that never happened) - now it is a duplicate,
    // naming the local at the call site.
    RC_CHECK_TRUE(ERR("FUNCTION f(n)\nr = 1\nr = r + n\n= r\nEQUB f(1)") == error_type_duplicate_symbol);
    RC_CHECK(diag_payload(&fix->r, error_type_duplicate_symbol), ==, RC_STR("r"));

    // Rebinding a parameter is the same mistake.
    RC_CHECK_TRUE(ERR("FUNCTION g(n)\nn = 2\n= n\nEQUB g(1)") == error_type_duplicate_symbol);

    // Branches stay fine: only the live arm binds, so IF/ELSE both assigning is one binding.
    RC_CHECK_TRUE(code_is(&fix->r, ASM(
        "FUNCTION pick(c)\nIF c\nx = 2\nELSE\nx = 3\nENDIF\n= x\n"
        "EQUB pick(TRUE), pick(FALSE)"), (uint8_t[]) {2, 3}, 2));
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

    uint32_t passes = (fix->r = assemble_string(&fix->desc, RC_STR("stress"), src.view)).passes;
    RC_CHECK_TRUE(passes != 0);
    RC_CHECK(baron_result_code(&fix->r).num, ==, 2000u);   // 2000 bytes from the loop; the labels emit nothing
    // A symbol bound last, after every relocation, is still correct...
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("last")), value_make_numeric(0xBEEF)));
    // ...as is a label from the middle of the run (they all resolve to the post-loop pc).
    RC_CHECK_TRUE(value_is_equal(baron_result_symbol(&fix->r, RC_STR("lbl250")), value_make_numeric(2000)));

    rc_arena_deinit(&build);
}

#endif // BARON_TESTS
