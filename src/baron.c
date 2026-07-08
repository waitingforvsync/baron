#include "baron.h"

#include "richc/macros.h"


enum { baron_per_pass_reserve = 64u * 1024 * 1024 };   // one pass of sections/macros/functions is small

baron_arenas baron_arenas_make(void)
{
    return (baron_arenas) {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make(baron_per_pass_reserve),
        .scratch   = rc_arena_make_default(),
    };
}

void baron_arenas_deinit(baron_arenas *a)
{
    RC_ASSERT(a != NULL);
    rc_arena_deinit(&a->permanent);
    rc_arena_deinit(&a->per_pass);
    rc_arena_deinit(&a->scratch);
}

baron baron_make(baron_arenas *a)
{
    RC_ASSERT(a != NULL);
    baron b;
    b.permanent = &a->permanent;   // borrowed: the arenas stay the caller's to free
    b.per_pass  = &a->per_pass;

    scopes_init(&b.scopes, &a->permanent);
    scopes_make_root(&b.scopes);              // the root is scope index 0
    sections_init(&b.sections, &a->per_pass);   // the default section is (re)made per pass by sections_reset
    zeropage_init(&b.zeropage, &a->permanent);   // empty + dormant; run_pass clears it, ZPRESERVE fills it
    source_files_init(&b.source_files, &a->permanent);
    macros_init(&b.macros, &a->per_pass);         // run_pass reseeds its store + token table each pass
    functions_init(&b.functions, &a->per_pass);   // ditto for the operand table

    b.include_depth  = 0;
    b.macro_depth    = 0;
    b.function_depth = 0;
    b.diagnostics    = rc_array_diagnostic_make(256, &a->permanent);
    return b;
}

void baron_error(baron *b, error_type code, cursor at)
{
    RC_ASSERT(b != NULL);
    rc_array_diagnostic_push(&b->diagnostics,
        (diagnostic) {.code = code, .at = at, .severity = severity_error}, b->permanent);
}

void baron_warning(baron *b, error_type code, cursor at, uint8_t severity)
{
    RC_ASSERT(b != NULL && severity != severity_error);   // a warning is a positive level; 0 would fail the assemble
    rc_array_diagnostic_push(&b->diagnostics,
        (diagnostic) {.code = code, .at = at, .severity = severity}, b->permanent);
}

uint32_t baron_error_count(const baron *b)
{
    RC_ASSERT(b != NULL);
    uint32_t count = 0;
    for (uint32_t i = 0; i < b->diagnostics.num; i++) {
        if (rc_view_diagnostic_get(b->diagnostics.view, i).severity == severity_error) {
            count++;
        }
    }
    return count;
}

bool baron_has_errors(const baron *b)
{
    return baron_error_count(b) > 0;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(baron, init_set_get)
{
    baron_arenas arenas = baron_arenas_make();
    baron b = baron_make(&arenas);

    // baron_make already made the root at scope index 0.
    RC_CHECK_TRUE(scopes_set_symbol(&b.scopes, 0, RC_STR("answer"), value_make_numeric(42.0), (cursor){0, 0}) == symbol_status_unchanged);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&b.scopes, 0, RC_STR("answer")), value_make_numeric(42.0)));

    baron_arenas_deinit(&arenas);
}

#endif // BARON_TESTS
