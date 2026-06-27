#ifndef BARON_ASSEMBLE_INTERNAL_H_
#define BARON_ASSEMBLE_INTERNAL_H_

#include "assemble.h"   // assemble_error, rc_str, rc_arena
#include "value.h"      // value (operand_value's input)


// The assembler's internal parsing vocabulary, shared between assemble.c (the statement loop and
// the directives) and opcodes.c (instruction parsing). Not part of the public assemble.h surface.

// The outputs of a parse, returned by value for the caller to fold into its own running state.
// `next` is the cursor past what was consumed; `unresolved` that some operand referenced a
// not-yet-defined symbol; `changed` that some existing symbol moved value (both feed
// convergence); `error`/`error_at` the first hard error.
typedef struct parse_result {
    uint32_t       next;
    bool           unresolved;
    bool           changed;
    assemble_error error;
    uint32_t       error_at;
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

// An operand reduced for emission. `known` carries the integer in `addr`; `unresolved` marks a
// forward reference to defer; `error` (with `error_at`) a value that can never be an address.
typedef struct operand {
    bool           known;
    int64_t        addr;
    bool           unresolved;
    assemble_error error;
    uint32_t       error_at;
} operand;

// Reduce an evaluated operand value to an integer for emission (pure: inputs in, result out). A
// plain numeric comes back `known`; a forward reference (unknown symbol) comes back not-known
// with `unresolved` set, to settle on a later pass; a value that can never be an address - or an
// unknown symbol on the final pass - comes back with `error` set. `at` is the offset to blame.
operand operand_value(value v, bool final_pass, uint32_t at);

// The separator (':' / newline / EOF) that must follow a non-label statement, starting at
// cursor. A following '}' counts as an implicit one (left for the scope to close). Returns the
// cursor past it in `.next`, or an error; `unresolved`/`changed` are always false. Defined in
// assemble.c (it reads the statement table); shared with opcodes.c.
parse_result require_separator(rc_str source, uint32_t cursor);


#endif // ifndef BARON_ASSEMBLE_INTERNAL_H_
