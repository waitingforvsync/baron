#include "baron.h"

#include "richc/macros.h"


void baron_init(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_init(&b->scopes);
    scopes_make_root(&b->scopes);   // the root is scope index 0
    b->code_arena = rc_arena_make_default();
    overlay_init(&b->overlay, &b->code_arena);
}

void baron_deinit(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_deinit(&b->scopes);
    rc_arena_deinit(&b->code_arena);
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(baron, init_set_get)
{
    baron b;
    baron_init(&b);

    // baron_init already made the root at scope index 0.
    RC_CHECK_FALSE(scopes_set_symbol(&b.scopes, 0, RC_STR("answer"), value_make_numeric(42.0)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&b.scopes, 0, RC_STR("answer")), value_make_numeric(42.0)));

    baron_deinit(&b);
}

#endif // BARON_TESTS
