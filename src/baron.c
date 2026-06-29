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
}

void baron_deinit(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_deinit(&b->scopes);
    overlays_deinit(&b->overlays);
    source_files_deinit(&b->source_files);
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
