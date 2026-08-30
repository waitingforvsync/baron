#ifndef BARON_ASSEMBLE_INTERNAL_H_
#define BARON_ASSEMBLE_INTERNAL_H_

#include "assemble.h"
#include "value.h"
#include "cursor.h"
#include "expression.h"


// The assembler's internal parsing vocabulary, shared between assemble.c (the statement loop and
// the directives) and opcodes.c (instruction parsing). Not part of the public assemble.h surface.

// baron is the internal machine (defined in baron.h); the public assemble.h no longer names it, so the
// shared parse vocabulary forward-declares it here for the handler signatures below.
typedef struct baron baron;

// Per-statement parse context, threaded by value alongside the cursor. The flags are independent,
// hence a struct not a bool: a dead IF branch is still parsed for structure (to find its ENDIF) but
// must effect nothing, and the output pass re-emits after allocation with diagnostics staying quiet
// (their gate is final, which it is not).
typedef struct parse_flags {
    bool final;     // the single armed pass after convergence: diagnostics record, the zp IR fills
    bool active;    // false inside a dead IF/FOR branch: parse for extent, effect nothing
    bool output;    // the post-allocation re-emission pass: ZA_AUTO symbols hold their real addresses
    bool listing;   // build the -v listing text (rides on the output pass; implies output)
} parse_flags;

// The outputs of a parse, returned by value for the caller to fold into its own running state. An
// error's code and location do NOT ride here - they are recorded straight into b->diagnostics at the
// failure site (syntax_error / semantic_error). unresolved and changed are independent (a pass can
// do both), so they stay two flags - not a tag - and the driver loops while either holds.
typedef struct parse_result {
    uint32_t next;         // cursor past what was consumed
    bool     fatal;        // a syntax error broke the token stream: the whole assemble unwinds
    bool     unresolved;   // some operand referenced a not-yet-defined symbol
    bool     changed;      // some existing symbol moved value
} parse_result;

// Record a fatal (syntax) error into b->diagnostics and yield the unwinding result: the token stream
// is broken, so the pass aborts. Checked on every pass (even in a dead branch), since syntax is.
// The _payload variant attaches the '%'-substituted string (see semantic_error_payload below).
parse_result syntax_error(baron *b, error_type code, cursor at);
parse_result syntax_error_payload(baron *b, error_type code, cursor at, rc_str payload);

// Record a recoverable (semantic) error into b->diagnostics - but ONLY on the settling pass and only
// when the branch is live (flags.final && flags.active), so convergence passes stay quiet and a dead
// branch raises nothing. The caller carries on with a best-effort emission; the error rides in the
// diagnostics array and fails the assemble at the end. The _payload variant attaches the string the
// renderer substitutes for '%' in the message (copied to permanent by the recorder - any backing).
void semantic_error(baron *b, parse_flags flags, error_type code, cursor at);
void semantic_error_payload(baron *b, parse_flags flags, error_type code, cursor at, rc_str payload);

// Record a warning into b->diagnostics at a positive severity level, gated exactly like semantic_error
// (settling pass, live branch). Unlike an error it does not fail the assemble - it just rides along for
// the caller to see (and to filter by warning level).
void semantic_warning(baron *b, parse_flags flags, error_type code, cursor at, uint8_t severity);

// An integer argument reduced for emission: one of three mutually-exclusive outcomes, so a single
// tag rather than a clutch of bools. za_auto marks a ZA_AUTO address: value then holds the OFFSET
// within the variable (the real base exists only after allocation) and zp_* carry its identity -
// callers needing a real number NOW must refuse (error_type_za_auto_address); the operand and
// data-emission paths accept, and the output pass re-evaluates against the allocated address.
typedef enum int_argument_type {
    int_argument_type_error,        // 0/default: a value that can never be an address (fail-safe)
    int_argument_type_known,        // value is a resolved integer
    int_argument_type_unresolved,   // a forward reference - defer to a later pass
} int_argument_type;

typedef struct int_argument {
    int64_t  value;          // valid when type == int_argument_type_known
    uint8_t  type;           // int_argument_type
    uint16_t error;          // error_type, set when type == int_argument_type_error
    uint32_t error_at;
    rc_str   error_detail;   // the error's payload (e.g. the undefined symbol's name), or {0}
    bool     za_auto;        // a ZA_AUTO address: value is the offset, zp_* its identity (see above)
    uint32_t zp_scope;
    cursor   zp_def;
    rc_str   zp_name;
} int_argument;

// Reduce an evaluated expression value to an integer argument (pure). Numeric -> known; a ZA_AUTO
// address -> known with the za_auto flag; a forward reference -> unresolved, settling later - or, on
// the final pass, an error, like any value that can never be an address. at is the offset to blame.
int_argument int_argument_make(value v, bool final_pass, uint32_t at);

// The separator (':' / newline / EOF) that must follow a non-label statement, starting at at. A
// following '}' counts as an implicit one (left for the scope to close). Returns the cursor past it
// in .next; a missing separator is a fatal (syntax) error, recorded into b and flagged in .fatal.
// Defined in assemble.c (it reads the statement table); shared with opcodes.c.
parse_result require_separator(baron *b, cursor at);

// Evaluate one expression in the assembler's current context: symbols from scope, plus the live PC of the
// current section. The single place that projects baron into an expr_env, so no call site rebuilds it.
// Defined in assemble.c (it reaches into b's sections); shared with opcodes.c.
expr_result eval(baron *b, cursor at, uint32_t scope, uint32_t section, rc_arena scratch);

// How a non-emitting statement sits in the listing: at the margin (a label, a scope brace, an
// assignment, SECTION framing - things that land nowhere), or with an address + empty byte field (a
// macro invocation, INCLUDE - the line marks where something lands).
typedef enum verbose_text_kind {
    verbose_text_margin = 0,
    verbose_text_address,
} verbose_text_kind;

// Append one line to the verbose listing - ONLY on the listing pass of a live branch (flags.listing
// && flags.active, the verbose twin of semantic_error's gate), the source sliced [stmt.pos, end_pos)
// and echoed verbatim, first line only. verbose_code_line is an emitting statement: address + hex
// dump (truncated after four bytes) + source, the bytes read back from the section from code_begin;
// verbose_text_line covers the rest, laid out by kind. Defined in assemble.c; shared with opcodes.c.
void verbose_code_line(baron *b, parse_flags flags, cursor stmt, uint32_t end_pos,
                       uint32_t section, uint32_t pc, uint32_t code_begin);
void verbose_text_line(baron *b, parse_flags flags, cursor stmt, uint32_t end_pos,
                       uint32_t pc, verbose_text_kind kind);


#endif // ifndef BARON_ASSEMBLE_INTERNAL_H_
