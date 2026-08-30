#include "functions.h"

#include "richc/macros.h"
#include "richc/mstr.h"


// Generous starting capacities so the per_pass arena rarely reallocates mid-pass. Floored at 8.
enum {
    functions_list_reserve      = 128,
    function_signatures_reserve = 8,
    function_params_reserve     = 8,
};

void functions_init(functions *f, rc_arena *per_pass)
{
    RC_ASSERT(f != NULL && per_pass != NULL);

    // The store stays empty here: functions_reset builds it and seeds the token table each pass.
    f->arena          = per_pass;
    f->list           = (rc_array_function) {0};
    f->operand_tokens = (rc_array_token) {0};
}


void functions_reset(functions *f, token_table base, uint32_t reserve_extra)
{
    RC_ASSERT(f != NULL);

    // The caller resets the shared per_pass arena once before this; we only re-make the containers.
    f->list           = rc_array_function_make(functions_list_reserve, f->arena);
    f->operand_tokens = rc_array_token_make_copy(base, base.num + reserve_extra, f->arena);
}


token_table functions_operand_tokens(const functions *f)
{
    RC_ASSERT(f != NULL);
    return f->operand_tokens.view;
}


static uint32_t functions_add(functions *f)
{
    function entry = { .signatures = rc_array_function_signature_make(function_signatures_reserve, f->arena) };
    return rc_array_function_push(&f->list, entry, f->arena);
}


uint32_t functions_index_for_name(functions *f, rc_str name)
{
    RC_ASSERT(f != NULL);

    // The operand token spells name( (the '(' baked in, like every builtin). Intern it so a call name(
    // matches regardless of any whitespace at the definition.
    rc_mstr spelling = rc_mstr_make(name.len + 2, f->arena);
    rc_mstr_append(&spelling, name, f->arena);
    rc_mstr_append_char(&spelling, '(', f->arena);

    uint32_t t = token_table_find(f->operand_tokens.view, spelling.view);
    if (t != RC_INDEX_NONE) {
        token tok = rc_view_token_get(f->operand_tokens.view, t);
        if (tok.lexeme.type == lexeme_type_user_function && tok.name.len == spelling.view.len) {
            return tok.lexeme.user_function.index;   // an overload, a self-call, or a fill-in: one entry per name
        }
    }

    uint32_t index = functions_add(f);
    rc_array_token_push(
        &f->operand_tokens,
        (token) {.name = spelling.view, .lexeme = {.type = lexeme_type_user_function, .user_function = {.index = index}}},
        f->arena);
    return index;
}


function *functions_at(functions *f, uint32_t index)
{
    RC_ASSERT(f != NULL);
    return rc_array_function_at(&f->list, index);
}


function_add_status functions_add_signature(functions *f, uint32_t index, rc_view_str params,
                                            cursor body, uint32_t def_scope, bool defined)
{
    function *e = functions_at(f, index);

    // Reconcile against an existing overload of the same arity (forward declaration -> fill; a second real
    // body -> duplicate; a repeated forward -> a harmless no-op).
    for (uint32_t i = 0; i < e->signatures.num; i++) {
        function_signature *sig = rc_array_function_signature_at(&e->signatures, i);
        if (sig->params.num == params.num) {
            if (!defined) {
                return function_add_redundant;
            }
            if (!sig->defined) {
                sig->params    = params;      // the real definition supplies the parameter names too
                sig->body      = body;
                sig->def_scope = def_scope;
                sig->defined   = true;
                return function_add_filled;
            }
            return function_add_duplicate;    // two real bodies for one arity
        }
    }

    rc_array_function_signature_push(
        &e->signatures,
        (function_signature) {
            .params    = params,
            .body      = body,
            .def_scope = def_scope,
            .defined   = defined,
        },
        f->arena);
    return function_add_inserted;
}


const function_signature *functions_match(const functions *f, uint32_t index, uint32_t argc)
{
    RC_ASSERT(f != NULL);

    const function *fn = rc_view_function_at(f->list.view, index);
    for (uint32_t i = 0; i < fn->signatures.view.num; i++) {
        const function_signature *sig = rc_view_function_signature_at(fn->signatures.view, i);
        if (sig->params.num == argc) {
            return sig;
        }
    }

    return NULL;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// A parameter-name view over a caller-owned array (arity is all the reconcile / match logic reads).
static rc_view_str t_params(const rc_str *names, uint32_t n)
{
    return (rc_view_str) {.data = names, .num = n};
}

RC_TEST(functions, index_for_name_reuses)
{
    rc_arena arena = rc_arena_make_default();
    functions f;
    functions_init(&f, &arena);
    functions_reset(&f, (token_table) {0}, 8);   // an empty base is fine for the store-only checks

    uint32_t a  = functions_index_for_name(&f, RC_STR("sqr"));
    uint32_t a2 = functions_index_for_name(&f, RC_STR("sqr"));   // same name -> same index (an overload / self-call)
    uint32_t b  = functions_index_for_name(&f, RC_STR("gcd"));
    RC_CHECK(a, ==, a2);
    RC_CHECK_TRUE(a != b);
    RC_CHECK(f.list.num, ==, 2u);

    rc_arena_deinit(&arena);
}

RC_TEST(functions, add_signature_reconciles_by_arity)
{
    rc_str one[]  = {RC_STR("x")};
    rc_str two[]  = {RC_STR("a"), RC_STR("b")};

    rc_arena arena = rc_arena_make_default();
    functions f;
    functions_init(&f, &arena);
    functions_reset(&f, (token_table) {0}, 8);
    uint32_t g = functions_index_for_name(&f, RC_STR("g"));

    // Distinct arities coexist.
    RC_CHECK_TRUE(functions_add_signature(&f, g, t_params(one, 1), (cursor){0, 10}, 0, true)  == function_add_inserted);
    RC_CHECK_TRUE(functions_add_signature(&f, g, t_params(two, 2), (cursor){0, 20}, 0, true)  == function_add_inserted);
    RC_CHECK(functions_at(&f, g)->signatures.num, ==, 2u);

    // A forward declaration then its fill-in is not a duplicate; a second real body is.
    uint32_t h = functions_index_for_name(&f, RC_STR("h"));
    RC_CHECK_TRUE(functions_add_signature(&f, h, t_params(one, 1), (cursor){0, 0},  0, false) == function_add_inserted);
    RC_CHECK_TRUE(functions_add_signature(&f, h, t_params(one, 1), (cursor){0, 40}, 0, true)  == function_add_filled);
    RC_CHECK_TRUE(functions_add_signature(&f, h, t_params(one, 1), (cursor){0, 80}, 0, true)  == function_add_duplicate);

    rc_arena_deinit(&arena);
}

RC_TEST(functions, match_picks_by_arity)
{
    rc_str one[] = {RC_STR("w")};
    rc_str two[] = {RC_STR("w"), RC_STR("h")};

    rc_arena arena = rc_arena_make_default();
    functions f;
    functions_init(&f, &arena);
    functions_reset(&f, (token_table) {0}, 8);
    uint32_t area = functions_index_for_name(&f, RC_STR("area"));
    functions_add_signature(&f, area, t_params(one, 1), (cursor){0, 10}, 0, true);
    functions_add_signature(&f, area, t_params(two, 2), (cursor){0, 20}, 0, true);

    RC_CHECK(functions_match(&f, area, 1)->body.pos, ==, 10u);
    RC_CHECK(functions_match(&f, area, 2)->body.pos, ==, 20u);
    RC_CHECK_TRUE(functions_match(&f, area, 3) == NULL);

    rc_arena_deinit(&arena);
}

RC_TEST(functions, reset_empties)
{
    rc_arena arena = rc_arena_make_default();
    functions f;
    functions_init(&f, &arena);
    functions_reset(&f, (token_table) {0}, 8);
    functions_index_for_name(&f, RC_STR("a"));
    functions_index_for_name(&f, RC_STR("b"));
    RC_CHECK(f.list.num, ==, 2u);
    functions_reset(&f, (token_table) {0}, 8);
    RC_CHECK(f.list.num, ==, 0u);
    RC_CHECK(functions_index_for_name(&f, RC_STR("c")), ==, 0u);   // usable again
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
