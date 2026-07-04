#include "baron.h"

#include "richc/macros.h"


void baron_init(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_init(&b->scopes);
    scopes_make_root(&b->scopes);          // the root is scope index 0
    overlays_init(&b->overlays);
    b->current_overlay = overlays_make_default(&b->overlays);   // emit into the default overlay (index 0)
    source_files_init(&b->source_files);
    b->include_depth = 0;
    b->diag_arena  = rc_arena_make_default();
    b->diagnostics = rc_array_diagnostic_make(0, &b->diag_arena);
}

void baron_deinit(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_deinit(&b->scopes);
    overlays_deinit(&b->overlays);
    source_files_deinit(&b->source_files);
    rc_arena_deinit(&b->diag_arena);
}

void baron_error(baron *b, error_type code, cursor at)
{
    RC_ASSERT(b != NULL);
    rc_array_diagnostic_push(&b->diagnostics,
        (diagnostic) {.code = code, .at = at, .severity = severity_error}, &b->diag_arena);
}

void baron_warning(baron *b, error_type code, cursor at, uint8_t severity)
{
    RC_ASSERT(b != NULL && severity != severity_error);   // a warning is a positive level; 0 would fail the assemble
    rc_array_diagnostic_push(&b->diagnostics,
        (diagnostic) {.code = code, .at = at, .severity = severity}, &b->diag_arena);
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
    baron b;
    baron_init(&b);

    // baron_init already made the root at scope index 0.
    RC_CHECK_TRUE(scopes_set_symbol(&b.scopes, 0, RC_STR("answer"), value_make_numeric(42.0), (cursor){0, 0}) == symbol_status_unchanged);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&b.scopes, 0, RC_STR("answer")), value_make_numeric(42.0)));

    baron_deinit(&b);
}

#endif // BARON_TESTS
