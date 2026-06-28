#ifndef BARON_ASSEMBLE_INTERNAL_H_
#define BARON_ASSEMBLE_INTERNAL_H_

#include "assemble.h"   // assemble_error, rc_str, rc_arena
#include "value.h"      // value (int_argument_make's input)


// The assembler's internal parsing vocabulary, shared between assemble.c (the statement loop and
// the directives) and opcodes.c (instruction parsing). Not part of the public assemble.h surface.

// The outputs of a parse, returned by value for the caller to fold into its own running state.
// `next` is the cursor past what was consumed; `error`/`error_at` the first hard error;
// `unresolved` that some operand referenced a not-yet-defined symbol; `changed` that some existing
// symbol moved value. unresolved and changed are independent (a pass can do both at once), so they
// stay two flags - not a tag - and the driver loops while either holds. The bools sit last to pack.
typedef struct parse_result {
    uint32_t       next;
    assemble_error error;
    uint32_t       error_at;
    bool           unresolved;
    bool           changed;
} parse_result;

// Build a failed parse_result, parking the cursor at the error offset.
static inline parse_result parse_fail(assemble_error error, uint32_t at)
{
    return (parse_result) {
        .next = at,
        .error = error,
        .error_at = at
    };
}

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
    assemble_error    error;      // set when type == int_argument_type_error
    uint32_t          error_at;
} int_argument;

// Reduce an evaluated expression value to an integer argument (pure: inputs in, result out). A plain
// numeric comes back known; a forward reference (unknown symbol) comes back unresolved, to settle on
// a later pass; a value that can never be an address - or an unknown symbol on the final pass - comes
// back as an error. `at` is the offset to blame.
int_argument int_argument_make(value v, bool final_pass, uint32_t at);

// The separator (':' / newline / EOF) that must follow a non-label statement, starting at
// cursor. A following '}' counts as an implicit one (left for the scope to close). Returns the
// cursor past it in `.next`, or an error; `unresolved`/`changed` are always false. Defined in
// assemble.c (it reads the statement table); shared with opcodes.c.
parse_result require_separator(rc_str source, uint32_t cursor);


#endif // ifndef BARON_ASSEMBLE_INTERNAL_H_
