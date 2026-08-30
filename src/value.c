#include "value.h"

#include "richc/macros.h"
#include <math.h>


value value_make_none(void)
{
    return (value) {.type = value_type_none};
}

value value_make_numeric(double n)
{
    return (value) {.type = value_type_numeric, .numeric = n};
}

// --beebasm-true: truth spelt -1.0 (BBC BASIC's all-bits-set), constant for a whole run.
static bool beebasm_true;

void value_set_beebasm_true(bool enable)
{
    beebasm_true = enable;
}

value value_make_bool(bool b)
{
    // The payload rides in .numeric as a canonical 1.0 / 0.0 (-1.0 in BeebAsm mode), so numeric
    // contexts (arithmetic, comparisons, int_argument) read a bool without a conversion step.
    // Every consumer tests truth as != 0.0, never == 1.0, so either spelling flows unchanged.
    return (value) {.type = value_type_boolean, .numeric = b ? (beebasm_true ? -1.0 : 1.0) : 0.0};
}

value value_make_string(rc_str s)
{
    return (value) {.type = value_type_string, .string = s};
}

value value_make_error(error_type e)
{
    return (value) {.type = value_type_error, .error = {.code = e}};
}

value value_make_error_detail(error_type e, rc_str detail)
{
    return (value) {.type = value_type_error, .error = {.code = e, .detail = detail}};
}

value value_make_range(value_range r)
{
    return (value) {.type = value_type_range, .range = r};
}

value value_make_list(rc_view_value items)
{
    // A value is a non-owning handle, so we just wrap the view: the elements stay
    // wherever the caller built them (a scratch arena, say). Promoting a list to
    // permanent storage is a deliberate deep value_clone done at the symbol-table
    // boundary, not something hidden in here.
    return (value) {.type = value_type_list, .list = items};
}

value value_make_za_auto(uint32_t scope, cursor def, int32_t offset, rc_str name)
{
    return (value) {.type = value_type_za_auto,
                    .za_auto = {.scope = scope, .def = def, .offset = offset, .name = name}};
}


value value_make_copy(value v, rc_arena *arena)
{
    switch (v.type) {
        case value_type_none:
        case value_type_numeric:
        case value_type_boolean:
        case value_type_range:
        case value_type_za_auto:   // the name view is into permanent source text, so no copy needed
            return v;   // wholly inline: nothing to deep-copy

        case value_type_error:
            // The detail is usually a view into source text, but error() composes its message in
            // the evaluation arena - so a bound error value must bring its detail's bytes along.
            if (v.error.detail.len != 0) {
                return value_make_error_detail(v.error.code, rc_mstr_from_str(v.error.detail, v.error.detail.len, arena).view);
            }
            return v;

        case value_type_string:
            // Copy the bytes so the copy no longer aliases the source/scratch text.
            return value_make_string(rc_mstr_from_str(v.string, v.string.len, arena).view);

        case value_type_list: {
            // make_copy gives an arena-backed array of the (shallow) handles; deep-copy each
            // element in place so nested lists and strings come along too.
            rc_array_value out = rc_array_value_make_copy(v.list, v.list.num, arena);
            for (uint32_t i = 0; i < out.num; i++) {
                rc_array_value_set(&out, i, value_make_copy(rc_view_value_get(v.list, i), arena));
            }
            return value_make_list(out.view);
        }
    }

    RC_UNREACHABLE();
}


// A range endpoint is a whole number. Pull the int64 out of v, or hand back the
// reason it cannot be one as an error value (none means success, *out is set). An
// error operand passes straight through so it can propagate.
static value range_int(value v, int64_t *out)
{
    if (value_is_error(v)) {
        return v;
    }
    if (!value_is_number(v)) {
        return value_make_error(error_type_type_mismatch);
    }
    double d = v.numeric;
    if (floor(d) != d || d < (double)INT64_MIN || d > (double)INT64_MAX) {
        return value_make_error(error_type_domain);   // not a whole number (or out of range / NaN)
    }
    *out = (int64_t)d;
    return value_make_none();
}

// The sign of x as -1 / 0 / +1.
static int64_t sign64(int64_t x)
{
    return (x > 0) - (x < 0);
}

value value_make_range_pair(value lhs, value rhs, bool exclusive)
{
    int64_t start;
    value err = range_int(lhs, &start);
    if (!value_is_none(err)) {
        return err;
    }

    if (value_is_range(rhs)) {
        // Stepped form a..(b..c): the inner range's start is our second element, so the
        // step is fixed at (second - start) and the inner end becomes our end.
        if (exclusive) {
            return value_make_error(error_type_domain);   // '<' must sit on the final separator
        }
        value_range r = rhs.range;
        if (!r.has_start) {
            return value_make_error(error_type_domain);   // ..(..x) makes no sense as a start
        }
        int64_t step = r.start - start;
        if (step == 0 || (r.step != 0 && r.step != step)) {
            return value_make_error(error_type_domain);   // zero or inconsistent step
        }
        if (!r.has_end) {
            return value_make_range((value_range) {.start = start, .step = step, .has_start = true});
        }
        if (sign64(r.end - r.start) != sign64(step)) {
            return value_make_error(error_type_domain);   // the end does not continue the same way
        }
        int64_t end = start + step * ((r.end - start) / step);   // canonical last element
        return value_make_range((value_range) {
            .start     = start,
            .end       = end,
            .step      = step,
            .has_start = true,
            .has_end   = true,
        });
    }

    // Simple form a..b: step stays 0 (the direction is inferred from the endpoints).
    int64_t end;
    err = range_int(rhs, &end);
    if (!value_is_none(err)) {
        return err;
    }
    if (exclusive) {
        if (start >= end) {
            return value_make_list((rc_view_value) {0});   // '..<' that does not ascend is a legal empty sequence
        }
        end -= 1;
    }
    return value_make_range((value_range) {
        .start     = start,
        .end       = end,
        .has_start = true,
        .has_end   = true,
    });
}

value value_make_range_open_end(value lhs, bool exclusive)
{
    if (exclusive) {
        return value_make_error(error_type_domain);   // 'a..<' has nothing to exclude
    }
    int64_t start;
    value err = range_int(lhs, &start);
    if (!value_is_none(err)) {
        return err;
    }
    return value_make_range((value_range) {.start = start, .has_start = true});
}

value value_make_range_open_start(value rhs, bool exclusive)
{
    int64_t end;
    value err = range_int(rhs, &end);   // a range here (..b..c) coerces to type_mismatch
    if (!value_is_none(err)) {
        return err;
    }
    return value_make_range((value_range) {.end = end - (exclusive ? 1 : 0), .has_end = true});
}

value value_make_range_open(bool exclusive)
{
    (void)exclusive;   // '..<' with neither endpoint is just the fully-open range
    return value_make_range((value_range) {0});
}

int64_t value_range_step(value_range r)
{
    if (r.step != 0) {
        return r.step;
    }
    // A stored step of 0 means "infer the direction": descend only when both ends are
    // present and the end sits below the start; otherwise (including any open end) ascend.
    return (r.has_start && r.has_end && r.end < r.start) ? -1 : 1;
}


value_type value_type_of(value v)  { return v.type; }
bool value_is_none(value v)        { return v.type == value_type_none; }
bool value_is_numeric(value v)     { return v.type == value_type_numeric; }
bool value_is_boolean(value v)     { return v.type == value_type_boolean; }
bool value_is_number(value v)      { return value_is_numeric(v) || value_is_boolean(v); }
bool value_is_string(value v)      { return v.type == value_type_string; }
bool value_is_list(value v)        { return v.type == value_type_list; }
bool value_is_range(value v)       { return v.type == value_type_range; }
bool value_is_error(value v)       { return v.type == value_type_error; }
bool value_is_za_auto(value v)      { return v.type == value_type_za_auto; }

// Simple values stand alone (a number, a boolean, a string, an error, a za_auto address); compound
// values gather others (a list of values, a range that enumerates to one). none belongs to neither.
// za_auto and boolean being simple is load-bearing: it routes them through the broadcast machinery
// as scalars, reaching the operator handlers (whose type checks accept or refuse them) instead of
// the list paths.
bool value_is_simple(value v)      { return value_is_number(v) || value_is_string(v) || value_is_error(v) || value_is_za_auto(v); }
bool value_is_compound(value v)    { return value_is_list(v) || value_is_range(v); }


bool value_is_equal(value a, value b)
{
    if (a.type != b.type) {
        return false;
    }

    switch (a.type) {
        case value_type_none:
            return true;
        case value_type_numeric:
        case value_type_boolean:   // canonical 1.0 / 0.0 payload, so the numeric compare is exact
            return a.numeric == b.numeric;
        case value_type_string:
            return rc_str_is_equal(a.string, b.string);
        case value_type_error:
            return a.error.code == b.error.code;   // detail is descriptive, not identity
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
        case value_type_za_auto:
            // Identity plus offset - the name is descriptive. This equality is what lets a derived
            // binding (x = var + 1) report `unchanged` pass after pass, so the assemble converges.
            return a.za_auto.scope == b.za_auto.scope
                && cursor_is_equal(a.za_auto.def, b.za_auto.def)
                && a.za_auto.offset == b.za_auto.offset;
    }

    RC_UNREACHABLE();
}




void value_format(rc_mstr *out, value v, rc_arena *arena)
{
    switch (v.type) {
        case value_type_none:
            rc_mstr_append(out, RC_STR("none"), arena);
            return;
        case value_type_numeric:
            rc_mstr_append_f64(out, v.numeric, arena);
            return;
        case value_type_boolean:
            rc_mstr_append(out, v.numeric != 0.0 ? RC_STR("TRUE") : RC_STR("FALSE"), arena);
            return;
        case value_type_string:
            rc_mstr_append_char(out, '"', arena);
            rc_mstr_append(out, v.string, arena);
            rc_mstr_append_char(out, '"', arena);
            return;
        case value_type_error:
            rc_mstr_append(out, RC_STR("<error: "), arena);
            error_append_message(out, v.error.code, v.error.detail, arena);
            rc_mstr_append_char(out, '>', arena);
            return;
        case value_type_range:
            if (v.range.has_start) {
                rc_mstr_append_i64(out, v.range.start, arena);
            }
            rc_mstr_append(out, RC_STR(".."), arena);
            if (v.range.step != 1) {
                rc_mstr_append_i64(out, v.range.step, arena);
                rc_mstr_append(out, RC_STR(".."), arena);
            }
            if (v.range.has_end) {
                rc_mstr_append_i64(out, v.range.end, arena);
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
        case value_type_za_auto:
            // Normally invisible - PRINT speaks on the output pass, where the symbol is a real number -
            // but an edge path (ERROR, say) may still render one before allocation.
            rc_mstr_append(out, RC_STR("<za_auto '"), arena);
            rc_mstr_append(out, v.za_auto.name, arena);
            rc_mstr_append_char(out, '\'', arena);
            if (v.za_auto.offset != 0) {
                rc_mstr_append_char(out, '+', arena);
                rc_mstr_append_i64(out, v.za_auto.offset, arena);
            }
            rc_mstr_append_char(out, '>', arena);
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

    value yes = value_make_bool(true);
    RC_CHECK_TRUE(value_is_boolean(yes));
    RC_CHECK_FALSE(value_is_numeric(yes));
    RC_CHECK_TRUE(value_is_number(yes));   // a bool coerces to a number...
    RC_CHECK_TRUE(value_is_simple(yes));   // ...and broadcasts as a scalar
    RC_CHECK(yes.numeric, ==, 1.0);
    RC_CHECK(value_make_bool(false).numeric, ==, 0.0);

    value str = value_make_string(RC_STR("hi"));
    RC_CHECK_TRUE(value_is_string(str));
    RC_CHECK(str.string, ==, RC_STR("hi"));

    value err = value_make_error(error_type_divide_by_zero);
    RC_CHECK_TRUE(value_is_error(err));
    RC_CHECK_FALSE(value_is_numeric(err));
}

RC_TEST(value, beebasm_true_mode)
{
    // --beebasm-true swaps the true payload to -1.0; false and everything downstream
    // (equality, truth tests) are payload-agnostic. Restore the default before leaving.
    value_set_beebasm_true(true);
    RC_CHECK(value_make_bool(true).numeric, ==, -1.0);
    RC_CHECK(value_make_bool(false).numeric, ==, 0.0);
    RC_CHECK_TRUE(value_is_equal(value_make_bool(true), value_make_bool(true)));
    value_set_beebasm_true(false);
    RC_CHECK(value_make_bool(true).numeric, ==, 1.0);
}

RC_TEST(value, scalar_equality)
{
    RC_CHECK_TRUE(value_is_equal(value_make_numeric(1), value_make_numeric(1)));
    RC_CHECK_FALSE(value_is_equal(value_make_numeric(1), value_make_numeric(2)));
    RC_CHECK_TRUE(value_is_equal(value_make_bool(true), value_make_bool(true)));
    RC_CHECK_FALSE(value_is_equal(value_make_bool(true), value_make_bool(false)));
    // Different types are never equal - TRUE and numeric 1 included: the symbol table's
    // convergence gate must notice a binding changing type, or the stale type would stick.
    RC_CHECK_FALSE(value_is_equal(value_make_bool(true), value_make_numeric(1)));
    RC_CHECK_FALSE(value_is_equal(value_make_numeric(0), value_make_none()));
    RC_CHECK_TRUE(value_is_equal(value_make_none(), value_make_none()));
    RC_CHECK_TRUE(value_is_equal(value_make_string(RC_STR("a")), value_make_string(RC_STR("a"))));
    RC_CHECK_FALSE(value_is_equal(value_make_string(RC_STR("a")), value_make_string(RC_STR("b"))));
}

RC_TEST(value, range_equality)
{
    value_range a = {.start = 0, .end = 10, .step = 1, .has_start = true, .has_end = true};
    value_range b = a;
    RC_CHECK_TRUE(value_is_equal(value_make_range(a), value_make_range(b)));
    b.end = 9;
    RC_CHECK_FALSE(value_is_equal(value_make_range(a), value_make_range(b)));
}

RC_TEST(value, list_equality)
{
    value elems[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3)};
    rc_view_value view = (rc_view_value) RC_VIEW(elems);
    value list = value_make_list(view);
    RC_CHECK_TRUE(value_is_list(list));
    RC_CHECK(list.list.num, ==, 3u);

    // Equal to an independently-built list over the same elements.
    RC_CHECK_TRUE(value_is_equal(list, value_make_list(view)));

    // Unequal to a shorter list, and to one with a differing element.
    value short_elems[] = {value_make_numeric(1), value_make_numeric(2)};
    value shorter = value_make_list((rc_view_value) RC_VIEW(short_elems));
    RC_CHECK_FALSE(value_is_equal(list, shorter));

    value diff_elems[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(4)};
    value different = value_make_list((rc_view_value) RC_VIEW(diff_elems));
    RC_CHECK_FALSE(value_is_equal(list, different));

    // Nested lists compare recursively.
    value outer_a[] = {list};
    value outer_b[] = {value_make_list(view)};
    value nested_a = value_make_list((rc_view_value) RC_VIEW(outer_a));
    value nested_b = value_make_list((rc_view_value) RC_VIEW(outer_b));
    RC_CHECK_TRUE(value_is_equal(nested_a, nested_b));
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
    value_format(&out, value_make_bool(true), &arena);
    RC_CHECK(out.view, ==, RC_STR("TRUE"));

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_bool(false), &arena);
    RC_CHECK(out.view, ==, RC_STR("FALSE"));

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_string(RC_STR("hi")), &arena);
    RC_CHECK(out.view, ==, RC_STR("\"hi\""));

    out = rc_mstr_make(16, &arena);
    value_format(&out, value_make_error(error_type_type_mismatch), &arena);
    RC_CHECK(out.view, ==, RC_STR("<error: Incompatible types>"));

    // Stepped, fully-bounded range.
    out = rc_mstr_make(16, &arena);
    value_range stepped = {.start = 0, .end = 10, .step = 2, .has_start = true, .has_end = true};
    value_format(&out, value_make_range(stepped), &arena);
    RC_CHECK(out.view, ==, RC_STR("0..2..10"));

    // Boundless start, step 1.
    out = rc_mstr_make(16, &arena);
    value_range upto = {.end = 9, .step = 1, .has_end = true};
    value_format(&out, value_make_range(upto), &arena);
    RC_CHECK(out.view, ==, RC_STR("..9"));

    // List, formatted recursively.
    out = rc_mstr_make(16, &arena);
    value elems[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3)};
    value list = value_make_list((rc_view_value) RC_VIEW(elems));
    value_format(&out, list, &arena);
    RC_CHECK(out.view, ==, RC_STR("{1, 2, 3}"));

    rc_arena_deinit(&arena);
}

RC_TEST(value, clone_string_survives_scratch)
{
    rc_arena permanent = rc_arena_make_default();
    rc_arena scratch   = rc_arena_make_default();

    // Build a string in scratch, then promote it into the permanent arena.
    rc_mstr m = rc_mstr_make(8, &scratch);
    rc_mstr_append(&m, RC_STR("hello"), &scratch);
    value original = value_make_string(m.view);
    value cloned   = value_make_copy(original, &permanent);

    // Wipe scratch out from under the original: the clone keeps its own backing.
    rc_arena_reset(&scratch);
    RC_CHECK(cloned.string, ==, RC_STR("hello"));
    RC_CHECK_TRUE(cloned.string.data != original.string.data);   // distinct storage (pointers only, not derefs)

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&permanent);
}

RC_TEST(value, clone_list_is_deep)
{
    rc_arena permanent = rc_arena_make_default();
    rc_arena scratch   = rc_arena_make_default();

    // {1, {2, 3}} built entirely in scratch.
    rc_array_value inner = {0};
    rc_array_value_push(&inner, value_make_numeric(2), &scratch);
    rc_array_value_push(&inner, value_make_numeric(3), &scratch);
    rc_array_value outer = {0};
    rc_array_value_push(&outer, value_make_numeric(1), &scratch);
    rc_array_value_push(&outer, value_make_list(inner.view), &scratch);
    value cloned = value_make_copy(value_make_list(outer.view), &permanent);

    // An independently-built equal value, in the permanent arena.
    rc_array_value ref_inner = {0};
    rc_array_value_push(&ref_inner, value_make_numeric(2), &permanent);
    rc_array_value_push(&ref_inner, value_make_numeric(3), &permanent);
    rc_array_value ref_outer = {0};
    rc_array_value_push(&ref_outer, value_make_numeric(1), &permanent);
    rc_array_value_push(&ref_outer, value_make_list(ref_inner.view), &permanent);
    value reference = value_make_list(ref_outer.view);

    rc_arena_reset(&scratch);   // strand the source storage
    RC_CHECK_TRUE(value_is_equal(cloned, reference));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&permanent);
}

RC_TEST(value, list_is_rc_view_value)
{
    // value_list must be the real rc_view_value, not a look-alike.
    RC_CHECK((uint32_t)sizeof(value_list), ==, (uint32_t)sizeof(rc_view_value));
    value_list vl = {.data = NULL, .num = 0};
    rc_view_value rv = vl;            // compiles only if they are the same type
    RC_CHECK_TRUE(rv.data == NULL);
}

#endif // BARON_TESTS
