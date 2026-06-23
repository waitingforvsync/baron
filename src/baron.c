#include "baron.h"

#include "richc/macros.h"


void baron_init(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_init(&b->scopes);
}

void baron_deinit(baron *b)
{
    RC_ASSERT(b != NULL);
    scopes_deinit(&b->scopes);
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(baron, init_set_get)
{
    baron b;
    baron_init(&b);

    uint32_t root = scopes_make_root(&b.scopes);
    RC_CHECK_FALSE(scopes_set_symbol(&b.scopes, root, RC_STR("answer"), value_make_numeric(42.0)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&b.scopes, root, RC_STR("answer")), value_make_numeric(42.0)));

    baron_deinit(&b);
}

#endif // BARON_TESTS
