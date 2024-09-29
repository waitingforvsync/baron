#include "base/test.h"
#include "base/allocator.h"
#include "base/str.h"

DEF_TEST(strview, comparison) {
    REQUIRE_TRUE(strview_equal(STRVIEW("Hello"), STRVIEW("Hello")));
    REQUIRE_FALSE(strview_equal(STRVIEW("Not Hello"), STRVIEW("not hello")));
    REQUIRE_TRUE(strview_equal(STRVIEW(""), STRVIEW("")));

    const char *str1 = "Camel";
    REQUIRE_TRUE(strview_equal(make_strview(str1), STRVIEW("Camel")));

    REQUIRE(strview_compare(STRVIEW("cat"), STRVIEW("cat")),==,0);
    REQUIRE(strview_compare(STRVIEW("cat"), STRVIEW("caz")),<,0);
    REQUIRE(strview_compare(STRVIEW("cat"), STRVIEW("cap")),>,0);
    REQUIRE(strview_compare(STRVIEW("cat"), STRVIEW("catch")),<,0);
    REQUIRE(strview_compare(STRVIEW("cat"), STRVIEW("ca")),>,0);
    REQUIRE(strview_compare(STRVIEW("cat"), STRVIEW("")),>,0);
    REQUIRE(strview_compare(STRVIEW(""), STRVIEW("cat")),<,0);
    REQUIRE(strview_compare(STRVIEW(""), STRVIEW("")),==,0);
}

DEF_TEST(strview, find) {
    REQUIRE_TRUE(strview_startswith(STRVIEW("camel"), STRVIEW("cam")));
    REQUIRE_FALSE(strview_startswith(STRVIEW("camel"), STRVIEW("cat")));
    REQUIRE_FALSE(strview_startswith(STRVIEW("camel"), STRVIEW("camellips")));
    REQUIRE_TRUE(strview_startswith(STRVIEW("camel"), STRVIEW("")));

    REQUIRE_TRUE(strview_endswith(STRVIEW("camellips"), STRVIEW("lips")));
    REQUIRE_FALSE(strview_endswith(STRVIEW("camellips"), STRVIEW("slips")));
    REQUIRE_FALSE(strview_endswith(STRVIEW("lips"), STRVIEW("slips")));
    REQUIRE_TRUE(strview_endswith(STRVIEW("camellips"), STRVIEW("")));

    REQUIRE(strview_find_first(STRVIEW("bowl"), STRVIEW("bowl")),==,0);
    REQUIRE(strview_find_first(STRVIEW("bowl"), STRVIEW("owl")),==,1);
    REQUIRE(strview_find_first(STRVIEW("bowl"), STRVIEW("bow")),==,0);
    REQUIRE(strview_find_first(STRVIEW("mississippi"), STRVIEW("iss")),==,1);
    REQUIRE(strview_find_first(STRVIEW("my cow"), STRVIEW("owl")),==,invalid_index);
    REQUIRE(strview_find_first(STRVIEW("short"), STRVIEW("looooong")),==,invalid_index);
    REQUIRE(strview_find_first(STRVIEW("empty"), STRVIEW("")),==,0);
    REQUIRE(strview_find_first(STRVIEW(""), STRVIEW("")),==,0);

    REQUIRE_TRUE(strview_contains(STRVIEW("Albuquerque"), STRVIEW("buque")));
    REQUIRE_TRUE(strview_contains(STRVIEW("anything"), STRVIEW("")));
    REQUIRE_FALSE(strview_contains(STRVIEW("something"), STRVIEW("nothing")));

    REQUIRE(strview_find_last(STRVIEW("mississippi"), STRVIEW("iss")),==,4);
    REQUIRE(strview_find_last(STRVIEW("alpaca"), STRVIEW("a")),==,5);
    REQUIRE(strview_find_last(STRVIEW("alpaca"), STRVIEW("")),==,6);
    REQUIRE(strview_find_last(STRVIEW("short"), STRVIEW("loooooong")),==,invalid_index);
    REQUIRE(strview_find_last(STRVIEW(""), STRVIEW("")),==,0);
}

DEF_TEST(strview, substr) {
    REQUIRE(strview_left(STRVIEW("bumblebee"), 3),==,STRVIEW("bum"));
    REQUIRE(strview_left(STRVIEW("bumblebee"), 0),==,STRVIEW(""));
    REQUIRE(strview_left(STRVIEW("abcde"), 5),==,STRVIEW("abcde"));
    REQUIRE(strview_left(STRVIEW("abcde"), 10),==,STRVIEW("abcde"));

    REQUIRE(strview_right(STRVIEW("bumblebee"), 3),==,STRVIEW("bee"));
    REQUIRE(strview_right(STRVIEW("bumblebee"), 0),==,STRVIEW(""));
    REQUIRE(strview_right(STRVIEW("abcde"), 5),==,STRVIEW("abcde"));
    REQUIRE(strview_right(STRVIEW("abcde"), 10),==,STRVIEW("abcde"));

    REQUIRE(strview_substr(STRVIEW("abcde"), 1, 3),==,STRVIEW("bcd"));
    REQUIRE(strview_substr(STRVIEW("abcde"), 3, 2),==,STRVIEW("de"));
    REQUIRE(strview_substr(STRVIEW("abcde"), 3, 4),==,STRVIEW("de"));
    REQUIRE(strview_substr(STRVIEW("abcde"), 5, 2),==,STRVIEW(""));

    REQUIRE(strview_mid(STRVIEW("bumblebee"), 3),==,STRVIEW("blebee"));
    REQUIRE(strview_mid(STRVIEW("bumblebee"), 0),==,STRVIEW("bumblebee"));
    REQUIRE(strview_mid(STRVIEW("abcde"), 4),==,STRVIEW("e"));
    REQUIRE(strview_mid(STRVIEW("abcde"), 5),==,STRVIEW(""));

    REQUIRE(strview_remove_prefix(STRVIEW("potato"), STRVIEW("po")),==,STRVIEW("tato"));
    REQUIRE(strview_remove_prefix(STRVIEW("potato"), STRVIEW("spud")),==,STRVIEW("potato"));
    REQUIRE(strview_remove_prefix(STRVIEW("potato"), STRVIEW("")),==,STRVIEW("potato"));

    REQUIRE(strview_remove_suffix(STRVIEW("potato"), STRVIEW("to")),==,STRVIEW("pota"));
    REQUIRE(strview_remove_suffix(STRVIEW("potato"), STRVIEW("spud")),==,STRVIEW("potato"));
    REQUIRE(strview_remove_suffix(STRVIEW("potato"), STRVIEW("")),==,STRVIEW("potato"));
}

DEF_TEST(strview, split) {
    strview_t path = STRVIEW("path/to/somewhere");
    strview_pair_t split = strview_first_split(path, STRVIEW("/"));
    REQUIRE(split.first,==,STRVIEW("path"));
    REQUIRE(split.second,==,STRVIEW("to/somewhere"));
    path = split.second;
    split = strview_first_split(path, STRVIEW("/"));
    REQUIRE(split.first,==,STRVIEW("to"));
    REQUIRE(split.second,==,STRVIEW("somewhere"));
    path = split.second;
    split = strview_first_split(path, STRVIEW("/"));
    REQUIRE(split.first,==,STRVIEW("somewhere"));
    REQUIRE_FALSE(strview_is_valid(split.second));

    path = STRVIEW("path/to/somewhere");
    split = strview_last_split(path, STRVIEW("/"));
    REQUIRE(split.first,==,STRVIEW("path/to"));
    REQUIRE(split.second,==,STRVIEW("somewhere"));
    path = split.first;
    split = strview_last_split(path, STRVIEW("/"));
    REQUIRE(split.first,==,STRVIEW("path"));
    REQUIRE(split.second,==,STRVIEW("to"));
    path = split.first;
    split = strview_last_split(path, STRVIEW("/"));
    REQUIRE_FALSE(strview_is_valid(split.first));
    REQUIRE(split.second,==,STRVIEW("path"));
}

DEF_TEST(strview, parseint) {
    strview_parse_int_result_t result;

    result = strview_parse_int(STRVIEW("379"));
    REQUIRE(result.value,==,379);
    REQUIRE(result.length_parsed,==,3);

    result = strview_parse_int(STRVIEW("1234abcd"));
    REQUIRE(result.value,==,1234);
    REQUIRE(result.length_parsed,==,4);

    result = strview_parse_int(STRVIEW("-1024"));
    REQUIRE(result.value,==,-1024);
    REQUIRE(result.length_parsed,==,5);

    result = strview_parse_int(STRVIEW("-"));
    REQUIRE(result.value,==,0);
    REQUIRE(result.length_parsed,==,0);

    result = strview_parse_int(STRVIEW("nothing"));
    REQUIRE(result.value,==,0);
    REQUIRE(result.length_parsed,==,0);
}

DEF_TEST(strview, parsehex) {
    strview_parse_hex_result_t result;

    result = strview_parse_hex(STRVIEW("D00B1E5"));
    REQUIRE(result.value,==,0xD00B1E5);
    REQUIRE(result.length_parsed,==,7);
}

DEF_TEST(strview, parsefloat) {
    
}
