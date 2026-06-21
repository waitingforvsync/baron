#include "value.h"

#include "richc/macros.h"
#include <stdio.h>


value value_make_none(void)
{
    return (value){ .type = value_type_none };
}

value value_make_numeric(double n)
{
    return (value){ .type = value_type_numeric, .numeric = n };
}

value value_make_string(rc_str s)
{
    return (value){ .type = value_type_string, .string = s };
}

value value_make_error(value_error e)
{
    return (value){ .type = value_type_error, .error = e };
}

value value_make_range(value_range r)
{
    return (value){ .type = value_type_range, .range = r };
}

value value_make_list(rc_view_value items, rc_arena *arena)
{
    // Copy the elements so the list owns arena-stable storage independent of the
    // caller's buffer.
    rc_array_value copy = rc_array_value_make_copy(items, items.num, arena);
    return (value){ .type = value_type_list, .list = copy.view };
}


value_type value_type_of(value v)  { return v.type; }
bool value_is_none(value v)        { return v.type == value_type_none; }
bool value_is_numeric(value v)     { return v.type == value_type_numeric; }
bool value_is_string(value v)      { return v.type == value_type_string; }
bool value_is_list(value v)        { return v.type == value_type_list; }
bool value_is_range(value v)       { return v.type == value_type_range; }
bool value_is_error(value v)       { return v.type == value_type_error; }


bool value_is_equal(value a, value b)
{
    if (a.type != b.type) {
        return false;
    }

    switch (a.type) {
        case value_type_none:
            return true;
        case value_type_numeric:
            return a.numeric == b.numeric;
        case value_type_string:
            return rc_str_is_equal(a.string, b.string);
        case value_type_error:
            return a.error == b.error;
        case value_type_range:
            return a.range.start == b.range.start
                && a.range.end == b.range.end
                && a.range.step == b.range.step
                && a.range.has_start == b.range.has_start
                && a.range.has_end == b.range.has_end;
        case value_type_list:
            if (a.list.num != b.list.num) {
                return false;
            }
            for (uint32_t i = 0; i < a.list.num; i++) {
                if (!value_is_equal(a.list.data[i], b.list.data[i])) {
                    return false;
                }
            }
            return true;
    }

    RC_UNREACHABLE();
}


rc_str value_error_name(value_error e)
{
    switch (e) {
        case value_error_none:                 return RC_STR("none");
        case value_error_divide_by_zero:       return RC_STR("divide_by_zero");
        case value_error_domain:               return RC_STR("domain");
        case value_error_unknown_symbol:       return RC_STR("unknown_symbol");
        case value_error_type_mismatch:        return RC_STR("type_mismatch");
        case value_error_subscript_range:      return RC_STR("subscript_range");
        case value_error_incorrect_parameters: return RC_STR("incorrect_parameters");
        case value_error_not_implemented:      return RC_STR("not_implemented");
    }

    RC_UNREACHABLE();
}


// Append the decimal text of a signed integer to the mutable string. Built back
// to front without printf; the magnitude is taken in unsigned to avoid the
// INT64_MIN negation overflow. Belongs in richc's mstr eventually.
static void mstr_append_int(rc_mstr *out, int64_t value, rc_arena *arena)
{
    char buf[20];               // up to 19 digits for 2^63, plus a possible '-'
    uint32_t i = sizeof buf;
    uint64_t mag = (value < 0) ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    do {
        buf[--i] = (char)('0' + (mag % 10));
        mag /= 10;
    } while (mag != 0);
    if (value < 0) {
        buf[--i] = '-';
    }
    rc_mstr_append(out, rc_str_make(buf + i, sizeof buf - i), arena);
}


// Append a textual representation of a double. Interim: a fixed, compile-time-
// checked snprintf format, until richc owns proper number formatting.
static void mstr_append_double(rc_mstr *out, double value, rc_arena *arena)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%g", value);
    RC_ASSERT(n >= 0 && (uint32_t)n < sizeof buf);
    rc_mstr_append(out, rc_str_make(buf, (uint32_t)n), arena);
}


void value_format(rc_mstr *out, value v, rc_arena *arena)
{
    switch (v.type) {
        case value_type_none:
            rc_mstr_append(out, RC_STR("none"), arena);
            return;
        case value_type_numeric:
            mstr_append_double(out, v.numeric, arena);
            return;
        case value_type_string:
            rc_mstr_append_char(out, '"', arena);
            rc_mstr_append(out, v.string, arena);
            rc_mstr_append_char(out, '"', arena);
            return;
        case value_type_error:
            rc_mstr_append(out, RC_STR("<error: "), arena);
            rc_mstr_append(out, value_error_name(v.error), arena);
            rc_mstr_append_char(out, '>', arena);
            return;
        case value_type_range:
            if (v.range.has_start) {
                mstr_append_int(out, v.range.start, arena);
            }
            rc_mstr_append(out, RC_STR(".."), arena);
            if (v.range.step != 1) {
                mstr_append_int(out, v.range.step, arena);
                rc_mstr_append(out, RC_STR(".."), arena);
            }
            if (v.range.has_end) {
                mstr_append_int(out, v.range.end, arena);
            }
            return;
        case value_type_list:
            rc_mstr_append_char(out, '{', arena);
            for (uint32_t i = 0; i < v.list.num; i++) {
                if (i > 0) {
                    rc_mstr_append(out, RC_STR(", "), arena);
                }
                value_format(out, v.list.data[i], arena);
            }
            rc_mstr_append_char(out, '}', arena);
            return;
    }

    RC_UNREACHABLE();
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(value, constructors_and_predicates)
{
    value none = value_make_none();
    RC_CHECK_TRUE(value_is_none(none));
    RC_CHECK_TRUE(value_type_of(none) == value_type_none);

    value num = value_make_numeric(42.0);
    RC_CHECK_TRUE(value_is_numeric(num));
    RC_CHECK(num.numeric, ==, 42.0);

    value str = value_make_string(RC_STR("hi"));
    RC_CHECK_TRUE(value_is_string(str));
    RC_CHECK(str.string, ==, RC_STR("hi"));

    value err = value_make_error(value_error_divide_by_zero);
    RC_CHECK_TRUE(value_is_error(err));
    RC_CHECK_FALSE(value_is_numeric(err));
}

RC_TEST(value, scalar_equality)
{
    RC_CHECK_TRUE(value_is_equal(value_make_numeric(1), value_make_numeric(1)));
    RC_CHECK_FALSE(value_is_equal(value_make_numeric(1), value_make_numeric(2)));
    // Different types are never equal.
    RC_CHECK_FALSE(value_is_equal(value_make_numeric(0), value_make_none()));
    RC_CHECK_TRUE(value_is_equal(value_make_none(), value_make_none()));
    RC_CHECK_TRUE(value_is_equal(value_make_string(RC_STR("a")), value_make_string(RC_STR("a"))));
    RC_CHECK_FALSE(value_is_equal(value_make_string(RC_STR("a")), value_make_string(RC_STR("b"))));
}

RC_TEST(value, range_equality)
{
    value_range a = { .start = 0, .end = 10, .step = 1, .has_start = true, .has_end = true };
    value_range b = a;
    RC_CHECK_TRUE(value_is_equal(value_make_range(a), value_make_range(b)));
    b.end = 9;
    RC_CHECK_FALSE(value_is_equal(value_make_range(a), value_make_range(b)));
}

RC_TEST(value, list_equality)
{
    rc_arena arena = rc_arena_make_default();

    value elems[] = { value_make_numeric(1), value_make_numeric(2), value_make_numeric(3) };
    rc_view_value view = { .data = elems, .num = 3 };
    value list = value_make_list(view, &arena);
    RC_CHECK_TRUE(value_is_list(list));
    RC_CHECK(list.list.num, ==, 3u);

    // Equal to an independently-built copy of the same elements.
    RC_CHECK_TRUE(value_is_equal(list, value_make_list(view, &arena)));

    // Unequal to a shorter list, and to one with a differing element.
    value short_elems[] = { value_make_numeric(1), value_make_numeric(2) };
    value shorter = value_make_list((rc_view_value){ .data = short_elems, .num = 2 }, &arena);
    RC_CHECK_FALSE(value_is_equal(list, shorter));

    value diff_elems[] = { value_make_numeric(1), value_make_numeric(2), value_make_numeric(4) };
    value different = value_make_list((rc_view_value){ .data = diff_elems, .num = 3 }, &arena);
    RC_CHECK_FALSE(value_is_equal(list, different));

    // Nested lists compare recursively.
    value outer_a[] = { list };
    value outer_b[] = { value_make_list(view, &arena) };
    value nested_a = value_make_list((rc_view_value){ .data = outer_a, .num = 1 }, &arena);
    value nested_b = value_make_list((rc_view_value){ .data = outer_b, .num = 1 }, &arena);
    RC_CHECK_TRUE(value_is_equal(nested_a, nested_b));

    rc_arena_deinit(&arena);
}

RC_TEST(value, formatting)
{
    rc_arena arena = rc_arena_make_default();
    rc_mstr out;

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_none(), &arena);
    RC_CHECK(out.view, ==, RC_STR("none"));

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_numeric(3.5), &arena);
    RC_CHECK(out.view, ==, RC_STR("3.5"));

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_string(RC_STR("hi")), &arena);
    RC_CHECK(out.view, ==, RC_STR("\"hi\""));

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_error(value_error_type_mismatch), &arena);
    RC_CHECK(out.view, ==, RC_STR("<error: type_mismatch>"));

    // Stepped, fully-bounded range.
    out = rc_mstr_make(16, &arena);
    value_range stepped = { .start = 0, .end = 10, .step = 2, .has_start = true, .has_end = true };
    value_format(&out, value_make_range(stepped), &arena);
    RC_CHECK(out.view, ==, RC_STR("0..2..10"));

    // Boundless start, step 1.
    out = rc_mstr_make(16, &arena);
    value_range upto = { .end = 9, .step = 1, .has_end = true };
    value_format(&out, value_make_range(upto), &arena);
    RC_CHECK(out.view, ==, RC_STR("..9"));

    // List, formatted recursively.
    out = rc_mstr_make(16, &arena);
    value elems[] = { value_make_numeric(1), value_make_numeric(2), value_make_numeric(3) };
    value list = value_make_list((rc_view_value){ .data = elems, .num = 3 }, &arena);
    value_format(&out, list, &arena);
    RC_CHECK(out.view, ==, RC_STR("{1, 2, 3}"));

    rc_arena_deinit(&arena);
}

RC_TEST(value, format_int_edges)
{
    // Direct check of the integer formatter's tricky cases; the range formatting
    // test above only exercises small values.
    rc_arena arena = rc_arena_make_default();

    rc_mstr out = rc_mstr_make(16, &arena);
    mstr_append_int(&out, 0, &arena);
    mstr_append_int(&out, -7, &arena);
    RC_CHECK(out.view, ==, RC_STR("0-7"));

    out = rc_mstr_make(24, &arena);
    mstr_append_int(&out, INT64_MAX, &arena);
    RC_CHECK(out.view, ==, RC_STR("9223372036854775807"));

    out = rc_mstr_make(24, &arena);
    mstr_append_int(&out, INT64_MIN, &arena);
    RC_CHECK(out.view, ==, RC_STR("-9223372036854775808"));

    rc_arena_deinit(&arena);
}

RC_TEST(value, error_names)
{
    RC_CHECK(value_error_name(value_error_none), ==, RC_STR("none"));
    RC_CHECK(value_error_name(value_error_divide_by_zero), ==, RC_STR("divide_by_zero"));
    RC_CHECK(value_error_name(value_error_not_implemented), ==, RC_STR("not_implemented"));
}

RC_TEST(value, list_is_rc_view_value)
{
    // value_list must be the real rc_view_value, not a look-alike.
    RC_CHECK((uint32_t)sizeof(value_list), ==, (uint32_t)sizeof(rc_view_value));
    value_list vl = { .data = NULL, .num = 0 };
    rc_view_value rv = vl;            // compiles only if they are the same type
    RC_CHECK_TRUE(rv.data == NULL);
}

#endif // BARON_TESTS
