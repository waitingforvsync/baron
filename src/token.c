#include "token.h"


const token *token_table_find(token_table tt, rc_str text)
{
    const token *best = NULL;
    for (uint32_t i = 0; i < tt.num; i++) {
        rc_str name = tt.data[i].name;
        if (name.len <= text.len && rc_str_is_equal_insensitive(rc_str_left(text, name.len), name)) {
            if (!best || name.len > best->name.len) {
                best = &tt.data[i];
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
        { RC_STR("+"),   { .type = lexeme_type_binary_op } },
        { RC_STR("++"),  { .type = lexeme_type_unary_op } },
        { RC_STR("and"), { .type = lexeme_type_binary_op } },
    };
    token_table tt = { .data = toks, .num = (uint32_t)(sizeof toks / sizeof toks[0]) };

    // Longest prefix wins.
    const token *t = token_table_find(tt, RC_STR("++x"));
    RC_CHECK_TRUE(t != NULL);
    RC_CHECK(t->name, ==, RC_STR("++"));

    t = token_table_find(tt, RC_STR("+y"));
    RC_CHECK_TRUE(t != NULL);
    RC_CHECK(t->name, ==, RC_STR("+"));

    // Case-insensitive (keywords).
    t = token_table_find(tt, RC_STR("ANDY"));
    RC_CHECK_TRUE(t != NULL);
    RC_CHECK(t->name, ==, RC_STR("and"));

    // No match.
    RC_CHECK_TRUE(token_table_find(tt, RC_STR("xyz")) == NULL);
}

#endif // BARON_TESTS
