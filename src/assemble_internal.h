#ifndef BARON_ASSEMBLE_INTERNAL_H_
#define BARON_ASSEMBLE_INTERNAL_H_

#include "assemble.h"   // error_type, diagnostic, rc_str, rc_arena
#include "value.h"      // value (int_argument_make's input)
#include "cursor.h"     // cursor (an error's location, and require_separator's position)


// The assembler's internal parsing vocabulary, shared between assemble.c (the statement loop and
// the directives) and opcodes.c (instruction parsing). Not part of the public assemble.h surface.

// Per-statement parse context, threaded by value alongside the cursor. `final` arms the deferred
// checks (range, undefined-on-final) on the settling pass. `active` says whether a statement's
// effects apply: inside a false IF branch it is cleared, so the statement is parsed for structure
// (to find the matching ENDIF) but emits nothing, binds nothing and raises nothing. The two are
// independent, hence a struct not a bool.
typedef struct parse_flags {
    bool final;
    bool active;
} parse_flags;

// The outputs of a parse, returned by value for the caller to fold into its own running state.
// `next` is the cursor past what was consumed; `fatal` that a syntax error broke the token stream
// and the whole assemble must unwind; `unresolved` that some operand referenced a not-yet-defined
// symbol; `changed` that some existing symbol moved value. The error's code and location do NOT ride
// here - they are recorded straight into b->diagnostics at the failure site (see syntax_error /
// semantic_error), so a recoverable error leaves no trace in the result and the statement simply
// carries on. unresolved and changed are independent (a pass can do both), so they stay two flags -
// not a tag - and the driver loops while either holds. The bools sit last to pack.
typedef struct parse_result {
    uint32_t next;
    bool     fatal;
    bool     unresolved;
    bool     changed;
} parse_result;

// Record a fatal (syntax) error into b->diagnostics and yield the unwinding result: the token stream
// is broken, so the pass aborts. Checked on every pass (even in a dead branch), since syntax is.
parse_result syntax_error(baron *b, error_type code, cursor at);

// Record a recoverable (semantic) error into b->diagnostics - but ONLY on the settling pass and only
// when the branch is live (flags.final && flags.active), so convergence passes stay quiet and a dead
// branch raises nothing. The caller carries on with a best-effort emission; the error rides in the
// diagnostics array and fails the assemble at the end.
void semantic_error(baron *b, parse_flags flags, error_type code, cursor at);

// Record a warning into b->diagnostics, gated exactly like semantic_error (settling pass, live
// branch). Unlike an error it does not fail the assemble - it just rides along for the caller to see.
void semantic_warning(baron *b, parse_flags flags, error_type code, cursor at);

// An integer argument reduced for emission: one of three mutually-exclusive outcomes, so a single
// tag rather than a clutch of bools. `value` is the integer (valid only when known); `error` /
// `error_at` say why an argument can never be an address. The 8-byte value leads so the struct packs.
typedef enum int_argument_type {
    int_argument_type_error,        // 0/default: a value that can never be an address (fail-safe)
    int_argument_type_known,        // `value` is a resolved integer
    int_argument_type_unresolved,   // a forward reference - defer to a later pass
} int_argument_type;

typedef struct int_argument {
    int64_t           value;      // valid when type == int_argument_type_known
    int_argument_type type;
    error_type        error;      // set when type == int_argument_type_error
    uint32_t          error_at;
} int_argument;

// Reduce an evaluated expression value to an integer argument (pure: inputs in, result out). A plain
// numeric comes back known; a forward reference (unknown symbol) comes back unresolved, to settle on
// a later pass; a value that can never be an address - or an unknown symbol on the final pass - comes
// back as an error. `at` is the offset to blame.
int_argument int_argument_make(value v, bool final_pass, uint32_t at);

// The separator (':' / newline / EOF) that must follow a non-label statement, starting at `at`. A
// following '}' counts as an implicit one (left for the scope to close). Returns the cursor past it
// in `.next`; a missing separator is a fatal (syntax) error, recorded into b and flagged in `.fatal`.
// Defined in assemble.c (it reads the statement table); shared with opcodes.c.
parse_result require_separator(baron *b, cursor at);


#endif // ifndef BARON_ASSEMBLE_INTERNAL_H_
