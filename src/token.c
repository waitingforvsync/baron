#include "token.h"


uint32_t token_table_find(token_table tt, rc_str text)
{
    uint32_t best = RC_INDEX_NONE;
    for (uint32_t i = 0; i < tt.num; i++) {
        rc_str name = rc_view_token_get(tt, i).name;
        if (name.len <= text.len && rc_str_is_equal_insensitive(rc_str_left(text, name.len), name)) {
            if (best == RC_INDEX_NONE || name.len > rc_view_token_get(tt, best).name.len) {
                best = i;
            }
        }
    }
    return best;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(token, find_longest_match)
{
    static const token toks[] = {
        { RC_STR_INIT("+"),   { .type = lexeme_type_binary_op } },
        { RC_STR_INIT("++"),  { .type = lexeme_type_unary_op } },
        { RC_STR_INIT("and"), { .type = lexeme_type_binary_op } },
    };
    token_table tt = { .data = toks, .num = (uint32_t)(sizeof toks / sizeof toks[0]) };

    // Longest prefix wins.
    uint32_t i = token_table_find(tt, RC_STR("++x"));
    RC_CHECK_TRUE(i != RC_INDEX_NONE);
    RC_CHECK(rc_view_token_get(tt, i).name, ==, RC_STR("++"));

    i = token_table_find(tt, RC_STR("+y"));
    RC_CHECK_TRUE(i != RC_INDEX_NONE);
    RC_CHECK(rc_view_token_get(tt, i).name, ==, RC_STR("+"));

    // Case-insensitive (keywords).
    i = token_table_find(tt, RC_STR("ANDY"));
    RC_CHECK_TRUE(i != RC_INDEX_NONE);
    RC_CHECK(rc_view_token_get(tt, i).name, ==, RC_STR("and"));

    // No match.
    RC_CHECK_TRUE(token_table_find(tt, RC_STR("xyz")) == RC_INDEX_NONE);
}

#endif // BARON_TESTS
