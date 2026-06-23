#ifndef BARON_EXPRESSION_H_
#define BARON_EXPRESSION_H_

#include "scopes.h"   // scopes, value, rc_arena, rc_str


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
    expr_error_expected_open_paren,     // a function name was not followed by '('
    expr_error_expected_close_paren,    // a '(' or function argument was left open
} expr_error;


typedef struct expr_result {
    value      value;       // the evaluated value (may itself be an eval-error value)
    uint32_t   next;        // cursor just past the consumed text
    expr_error error;       // a parse error, or expr_error_none
    uint32_t   error_at;    // cursor where a parse error was spotted (when error set)
} expr_result;


// Parse and evaluate one expression from text starting at cursor, looking symbols
// up from scope_index in s. arena backs any value that needs to allocate (none of
// the numeric operators do yet). See the header comment for the greedy semantics.
expr_result expression_parse(rc_str text, uint32_t cursor,
                             const scopes *s, uint32_t scope_index, rc_arena *arena);


#endif // ifndef BARON_EXPRESSION_H_
