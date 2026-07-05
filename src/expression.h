#ifndef BARON_EXPRESSION_H_
#define BARON_EXPRESSION_H_

#include "value.h"   // value, rc_arena, rc_str

typedef struct scopes scopes;   // forward: expression_parse needs only the pointer


// The expression parser: a Pratt (precedence-climbing) parser that lexes an
// expression from source, resolves symbols against a scope as it goes, evaluates,
// and returns a value. Symbols that do not resolve, and arithmetic mishaps like
// divide-by-zero, become error values that propagate to the end rather than
// aborting the parse - so a forward reference can be retried on a later pass.
//
// The parser is greedy: it consumes as much as makes sense and then stops, handing
// back the cursor so the caller can carry on. It stops at the first lexeme it
// cannot use (a stray close paren, a trailing binary op with no operand) without
// complaint. But once it has committed to a bracketed construct - an open paren or
// a function call - a missing close paren or a function argument that never opens
// is a real error it reports with a position, because those are awkward to diagnose
// from the outside.

typedef enum expr_error {
    expr_error_none,                    // success, or a clean greedy stop
    expr_error_expected_expression,     // an operand was required but none was there
    expr_error_expected_close_paren,    // a '(' or function argument was left open
    expr_error_expected_close_brace,    // a list literal wanted a ',' or '}'
    expr_error_expected_close_bracket,  // a subscript '[' was left open
} expr_error;


typedef struct expr_result {
    value      value;       // the evaluated value (may itself be an eval-error value)
    uint32_t   next;        // cursor just past the consumed text
    expr_error error;       // a parse error, or expr_error_none
    uint32_t   error_at;    // cursor where a parse error was spotted (when error set)
} expr_result;


// The environment one expression evaluates in: symbol resolution plus the live assembler state an impure
// value needs. Passed by const pointer; the evaluator never sees `baron`. It grows fields over time (the
// PC's write offset, the current overlay's base, ...) without ever gaining a baron dependency.
typedef struct expr_env {
    const scopes *scopes;
    uint32_t      scope_index;
    uint32_t      pc;            // current program counter, for * / P%
    uint32_t      source;        // the reference's source, and...
    uint32_t      offset;        // ...its position, together the use site - for the local labels @- / @+
} expr_env;


// Parse and evaluate one expression from text starting at cursor, resolving symbols and reading live
// assembler state through env. arena backs any value that needs to allocate (none of the numeric operators
// do yet). See the header comment for the greedy semantics.
expr_result expression_parse(rc_str text, uint32_t cursor, const expr_env *env, rc_arena *arena);


// Reseed the RND stream to its fixed starting point. The multi-pass driver calls this at the start of every
// pass, so RND draws are reproducible pass-to-pass (and run-to-run) rather than drifting - which is what lets
// an assembly that uses RND converge. See RND / richc/random.h (rc_random).
void expression_reset_random(void);


// Enumerate a (bounded) range into a fresh list in arena, capped at VALUE_LIST_MAX_LENGTH. An
// unbounded range yields a domain error, an oversized one error_type_list_too_big. FOR uses this to
// walk a range sequence; internally it is also how the operators coerce a range to its elements.
value range_to_list(value_range r, rc_arena *arena);


#endif // ifndef BARON_EXPRESSION_H_
