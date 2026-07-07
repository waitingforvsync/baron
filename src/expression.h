#ifndef BARON_EXPRESSION_H_
#define BARON_EXPRESSION_H_

#include "value.h"   // value, rc_arena, rc_str
#include "token.h"   // token_table (the dynamic operand table carried in the env)

typedef struct scopes       scopes;         // forward: expression_parse needs only the pointers
typedef struct functions    functions;      // the user-FUNCTION registry (a call resolves its body through it)
typedef struct source_files source_files;   // a function body may live in a different source than the call


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
// value needs. Passed by const pointer; the evaluator never sees `baron`. It grows fields over time without
// ever gaining a baron dependency.
//
// `scopes` is now MUTABLE, because a user-FUNCTION call interprets its body inside the evaluator: it makes a
// per-call child scope and binds function-locals directly (symbols are immutable, so this stays purely
// functional). The registry + source cache + operand table + recursion counter are what that interpretation
// needs; all still read-only projections of `baron`.
typedef struct expr_env {
    scopes             *scopes;         // MUTABLE: a FUNCTION body binds locals / makes its child scope
    uint32_t            scope_index;
    uint32_t            pc;             // current program counter, for * / P%
    uint32_t            source;         // the reference's source, and...
    uint32_t            offset;         // ...its position, together the use site - for the local labels @- / @+
    token_table         operand_tokens; // dynamic operand (even) table: static base + a token per FUNCTION name;
                                        //   {0} means "use the static base" (expression.c's own unit tests)
    const functions    *functions;      // a lexeme_type_user_function index -> its signatures (params + body)
    const source_files *sources;        // to fetch a function body's source text (it may be a different file)
    uint32_t           *call_depth;     // FUNCTION recursion guard (points at baron.function_depth); may be NULL
} expr_env;

// The static base operand (even) table - mnemonic-free, just the builtin operators / functions / constants /
// brackets. The functions manager copies this each pass and appends a token per user function; the copy is
// threaded back in via expr_env.operand_tokens. handle_function also lexes a candidate name against it, to
// reject a name that collides with a builtin operand call (abs(, lo(, ...).
token_table expression_operand_base(void);

// Parse a FUNCTION body inactively from `pos` (just past the header ')') to locate its top-level '=' return.
// Used by handle_function at definition time: it needs the body's end and whether the definition is a real
// body or a forward declaration (an empty body AND an empty return expression).
typedef struct function_body_scan {
    uint32_t   next;         // just past the return expression (or the '=' for an empty return)
    bool       defined;      // false = forward declaration (empty body + empty return)
    error_type error;        // error_type_none, or a structural problem in the body
    uint32_t   error_at;     // where the structural problem was seen
} function_body_scan;

function_body_scan expression_scan_function_body(rc_str text, uint32_t pos, const expr_env *env, rc_arena *arena);


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
