#ifndef BARON_FUNCTIONS_H_
#define BARON_FUNCTIONS_H_

#include "token.h"
#include "cursor.h"


// The user-defined-FUNCTION store. A function is a name bound to one or more OVERLOADS (signatures),
// each a parameter list plus the cursor of the value-expression body it evaluates - rebuilt from
// source every pass (functions_reset), a transient projection rather than durable state. A function
// is NAMELESS here: its name( spelling lives in the dynamic operand table, carrying its index.

#include "richc/array/str.h"

// One overload: its parameter names, the body it evaluates, the scope it was defined in (the lexical parent
// of each call's child scope), and whether the body has actually been supplied. An empty body + empty return
// (FUNCTION name(params)=) is a forward declaration (defined == false); a later same-arity definition fills
// it in. params is a view into the manager arena (the builder is promoted to a view on registration).
typedef struct function_signature {
    rc_view_str    params;      // parameter names; arity = params.num
    cursor         body;        // just past ')'; the body runs to the top-level '=' return
    uint32_t       def_scope;   // the scope the FUNCTION was defined in (the lexical parent at each call)
    bool           defined;     // false = forward declaration (empty body + empty return); filled in later
} function_signature;

#define RC_ARRAY_TYPE function_signature
#define RC_ARRAY_NAME function_signature
#include "richc/template/array.h"

// All the overloads sharing one name. Overloads differ by arity (a call picks by argument count); two
// definitions of one arity are a duplicate. Insertion order is kept - matching is a linear arity scan.
typedef struct function {
    rc_array_function_signature signatures;
} function;

#define RC_ARRAY_TYPE function
#define RC_ARRAY_NAME function
#include "richc/template/array.h"

// The manager: one arena backs everything (the list, each function's signatures/param views, AND the dynamic
// operand-token table), all reset together each pass. statement_tokens' operand-table twin: the static base
// operand table (expression.c) plus one lexeme_type_user_function entry per function name, rebuilt in source
// order every pass, threaded to the evaluator through expr_env.
typedef struct functions {
    rc_arena         *arena;           // BORROWED: baron's per_pass arena (list, sub-arrays, tokens all live here)
    rc_array_function list;            // one per distinct name; a lexeme_type_user_function's index addresses it
    rc_array_token    operand_tokens;  // the live operand table (static base + a token per function name)
} functions;

void functions_init(functions *f, rc_arena *per_pass);

// Rebuild the store for a fresh pass and reseed operand_tokens from base (the static operand table) with
// room for reserve_extra function-name tokens. The caller resets the shared per_pass arena once BEFORE
// this (reclaiming the previous pass's list, sub-arrays and tokens); this only re-makes the containers.
void functions_reset(functions *f, token_table base, uint32_t reserve_extra);

// The live operand-token table the expression parser lexes calls from (the base plus a token per function).
token_table functions_operand_tokens(const functions *f);

// Map a function name to its list index, registering it on first sight: a new name appends a fresh (empty)
// function AND a lexeme_type_user_function token spelling name( (interned into the arena, so whitespace in
// the definition does not matter); an existing name - an overload, a self-call, or a forward declaration being
// filled - reuses its index. name is the bare name (no paren).
uint32_t functions_index_for_name(functions *f, rc_str name);

// A pointer to the function at index. Valid only until the list next grows - copy out what you need.
function *functions_at(functions *f, uint32_t index);

// The outcome of registering a signature on a name (mirrors the macro reconcile).
typedef enum function_add_status {
    function_add_inserted,    // a new overload (a new arity)
    function_add_filled,      // a real body supplied for a matching forward declaration
    function_add_redundant,   // an empty (forward) body for an already-known arity: a no-op
    function_add_duplicate,   // a second real body for an arity already defined
} function_add_status;

// Register {params, body, def_scope, defined} on function index, reconciling against its overloads by
// arity (parameter names do not distinguish overloads). params and body must outlive the pass.
function_add_status functions_add_signature(functions *f, uint32_t index, rc_view_str params,
                                            cursor body, uint32_t def_scope, bool defined);

// The signature of function index whose arity == argc, or NULL if none. Read-only (the caller reads its
// body / def_scope / defined without mutating).
const function_signature *functions_match(const functions *f, uint32_t index, uint32_t argc);


#endif // ifndef BARON_FUNCTIONS_H_
