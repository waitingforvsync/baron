#include "expression.h"

#include "scopes.h"   // scopes_get_symbol (expression.h only forward-declares scopes)
#include "lexer.h"
#include "richc/array/u32.h"   // rc_array_u32, for the indices a subscript selector picks
#include "richc/macros.h"
#include <math.h>


// ---- precedence ladder (higher binds tighter) ----
// The range operator '..' is the loosest, so its endpoints are whole expressions;
// comparisons and the logical ops sit below arithmetic; shifts share the multiply
// level; unary +/- sit below pow so -2^2 is -(2^2).
typedef enum prec {
    prec_lohi  = 1,    // unary < > (low/high byte); swallow the whole following expression
    prec_range = 5,    // ..  ..<  (right-associative; handled specially, not a binary_op)
    prec_or    = 10,   // or eor
    prec_and   = 20,   // and
    prec_cmp   = 30,   // = == != <> < > <= >=
    prec_add   = 40,   // + -
    prec_mul   = 50,   // * / div mod << >>
    prec_neg   = 60,   // unary + -
    prec_pow   = 70,   // ^  (right-associative)
    prec_subscript = 80,   // [..] postfix; binds tightest, so -L[0] is -(L[0])
} prec;


// ---- operator and function handlers ----
// The apply_* wrappers below screen operands for error values first, so a handler
// only ever sees real values. Every operator here wants numbers; NEEDS_NUM bails
// out with a type-mismatch otherwise. The arena is unused by the numeric operators
// but stays in the signature for the string/list operators still to come.

#define NEEDS_NUM(cond) do { if (!(cond)) return value_make_error(value_error_type_mismatch); } while (0)

// Coerce a number to 32 bits, truncating toward zero. The int64 hop makes the
// truncation well-defined before the 32-bit wrap. Bitwise ops read the value as
// unsigned (so >> is logical and NOT yields a positive pattern); div/mod read it
// signed.
static int32_t  as_i32(value v) { return (int32_t)(int64_t)v.numeric; }
static uint32_t as_u32(value v) { return (uint32_t)(int64_t)v.numeric; }

static value op_add(value a, value b, rc_arena *arena)
{
    if (value_is_string(a) && value_is_string(b)) {
        rc_mstr m = rc_mstr_make(a.string.len + b.string.len, arena);
        rc_mstr_append(&m, a.string, arena);
        rc_mstr_append(&m, b.string, arena);
        return value_make_string(m.view);   // '+' concatenates two strings
    }
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric + b.numeric);
}

static value op_sub(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric - b.numeric);
}

static value op_mul(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric * b.numeric);
}

static value op_div(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    if (b.numeric == 0.0) {
        return value_make_error(value_error_divide_by_zero);
    }
    return value_make_numeric(a.numeric / b.numeric);
}

static value op_pow(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    double r = pow(a.numeric, b.numeric);
    if (isnan(r)) {
        return value_make_error(value_error_domain);   // e.g. a negative base, fractional exponent
    }
    return value_make_numeric(r);
}

// Integer division and modulo, signed and truncating toward zero (C semantics).
static value op_idiv(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    int32_t ib = as_i32(b);
    if (ib == 0) {
        return value_make_error(value_error_divide_by_zero);
    }
    int32_t ia = as_i32(a);
    if (ia == INT32_MIN && ib == -1) {
        return value_make_numeric((double)INT32_MIN);   // sidestep the signed-overflow UB
    }
    return value_make_numeric((double)(ia / ib));
}

static value op_mod(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    int32_t ib = as_i32(b);
    if (ib == 0) {
        return value_make_error(value_error_divide_by_zero);
    }
    int32_t ia = as_i32(a);
    if (ia == INT32_MIN && ib == -1) {
        return value_make_numeric(0.0);                 // a mod -1 is 0; avoid the UB
    }
    return value_make_numeric((double)(ia % ib));
}

// Bitwise ops work on the unsigned 32-bit pattern. A shift count of 32 or more
// (which includes any negative count, having wrapped) yields zero.
static value op_shl(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    uint32_t n = as_u32(b);
    return value_make_numeric(n >= 32 ? 0.0 : (double)(as_u32(a) << n));
}

static value op_shr(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    uint32_t n = as_u32(b);
    return value_make_numeric(n >= 32 ? 0.0 : (double)(as_u32(a) >> n));
}

static value op_and(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric((double)(as_u32(a) & as_u32(b)));
}

static value op_or(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric((double)(as_u32(a) | as_u32(b)));
}

static value op_eor(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric((double)(as_u32(a) ^ as_u32(b)));
}

// Order two operands of the same kind - both numbers, or both strings (lexicographic) -
// as -1 / 0 / +1; sets *ok false on a type mismatch, which makes the comparison an error.
static int compare_values(value a, value b, bool *ok)
{
    *ok = true;
    if (value_is_numeric(a) && value_is_numeric(b)) {
        return (a.numeric > b.numeric) - (a.numeric < b.numeric);
    }
    if (value_is_string(a) && value_is_string(b)) {
        int c = rc_str_compare(a.string, b.string);
        return (c > 0) - (c < 0);
    }
    *ok = false;
    return 0;
}

// The comparisons each yield 1 for true, 0 for false, and read the ordering c from
// compare_values (so they all work on numbers or on strings). COMP is the test on c.
#define COMPARE_OP(name, COMP)                                            \
    static value name(value a, value b, rc_arena *arena)                  \
    {                                                                     \
        (void)arena;                                                      \
        bool ok;                                                          \
        int c = compare_values(a, b, &ok);                               \
        return ok ? value_make_numeric((COMP) ? 1.0 : 0.0)               \
                  : value_make_error(value_error_type_mismatch);         \
    }

COMPARE_OP(op_eq, c == 0)
COMPARE_OP(op_ne, c != 0)
COMPARE_OP(op_lt, c <  0)
COMPARE_OP(op_gt, c >  0)
COMPARE_OP(op_le, c <= 0)
COMPARE_OP(op_ge, c >= 0)

#undef COMPARE_OP

// The fold operators behind min()/max(); they are not in the tables, only used by reduce().
static value op_min(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric < b.numeric ? a.numeric : b.numeric);
}

static value op_max(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric > b.numeric ? a.numeric : b.numeric);
}

static value op_neg(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric(-v.numeric);
}

static value op_pos(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return v;
}

static value fn_abs(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric(fabs(v.numeric));
}

// Low and high bytes of the 32-bit pattern.
static value fn_lo(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric((double)(as_u32(v) & 0xFFu));
}

static value fn_hi(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric((double)((as_u32(v) >> 8) & 0xFFu));
}

static value fn_not(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric((double)(~as_u32(v)));
}

static value fn_sqrt(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    double r = sqrt(v.numeric);
    if (isnan(r)) {
        return value_make_error(value_error_domain);   // negative argument
    }
    return value_make_numeric(r);
}

// Rounding to an integral value, the three directions: down, toward zero, up.
static value fn_int(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric(floor(v.numeric));
}

static value fn_round(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric(trunc(v.numeric));
}

static value fn_ceil(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric(ceil(v.numeric));
}

// Apply a one-argument C math function, turning a NaN result (a domain error such as
// asin(2) or ln(-1)) into a domain error value. The trig/log builtins are thin wrappers.
static value math1(double (*f)(double), value v)
{
    NEEDS_NUM(value_is_numeric(v));
    double r = f(v.numeric);
    if (isnan(r)) {
        return value_make_error(value_error_domain);
    }
    return value_make_numeric(r);
}

static value fn_sin(value v, rc_arena *arena)  { (void)arena; return math1(sin,   v); }
static value fn_cos(value v, rc_arena *arena)  { (void)arena; return math1(cos,   v); }
static value fn_tan(value v, rc_arena *arena)  { (void)arena; return math1(tan,   v); }
static value fn_asin(value v, rc_arena *arena) { (void)arena; return math1(asin,  v); }
static value fn_acos(value v, rc_arena *arena) { (void)arena; return math1(acos,  v); }
static value fn_atan(value v, rc_arena *arena) { (void)arena; return math1(atan,  v); }
static value fn_log(value v, rc_arena *arena)  { (void)arena; return math1(log10, v); }   // base 10
static value fn_ln(value v, rc_arena *arena)   { (void)arena; return math1(log,   v); }   // natural
static value fn_exp(value v, rc_arena *arena)  { (void)arena; return math1(exp,   v); }

#undef NEEDS_NUM

// The range operator handlers. Unlike the operators above they live nowhere in the
// tables (range is lexeme_type_range, not a binary_op): parse_precedence has its own '..'
// case that calls one of these directly, at prec_range and right-associative. A range
// builds from its raw operands - no broadcast, no coercion - and value_make_range_pair
// owns the semantics (and the error propagation).
static value op_range(value a, value b, rc_arena *arena)
{
    (void)arena;
    return value_make_range_pair(a, b, false);
}

static value op_range_excl(value a, value b, rc_arena *arena)
{
    (void)arena;
    return value_make_range_pair(a, b, true);
}


// ---- list shape ----
// A list's shape is its axis lengths outermost-first; this backs shape() and the rank()
// used by broadcasting. Rough first cut, to be refined later (here, not in value.c).

#define MAX_RANK 32   // shape is truncated past this depth; far beyond any real list

// The uniform-length-prefix shape of v written into dims[], returning its rank: descend
// while every node at a level is a list of the same length, stopping at the first axis
// that is not uniform (so a ragged list reports the axes that ARE uniform). A scalar is
// rank 0; a leading length-1 axis is kept, not squashed.
//
// The whole thing rests on one recursive idea: a list's shape is its own length, followed
// by the shape that ALL of its elements agree on. So {{1,2},{3,4}} is 2-of-(things that
// are each {2}) -> {2,2}; but {{1,2},{3,4,5}} is 2-of-(a {2} and a {3}, which agree on
// nothing) -> just {2}. "What they all agree on" is the common leading prefix of the
// elements' shapes, and each element can only ever trim that prefix shorter.
static uint32_t shape_dims(value v, uint32_t dims[], uint32_t max)
{
    if (!value_is_list(v) || max == 0) {
        return 0;   // a scalar (or we have run out of room): empty shape, rank 0
    }
    dims[0] = v.list.num;   // axis 0 is always just how many things we are holding
    if (v.list.num == 0) {
        return 1;   // empty list: shape {0}, with no elements to descend into
    }

    // Element 0 proposes the inner shape. We let it write straight into our own buffer at
    // dims+1, so dims ends up as [our length, elem0's shape...] - if everyone agrees, that
    // IS the answer and nobody need touch it again.
    uint32_t inner = shape_dims(rc_view_value_get(v.list, 0), dims + 1, max - 1);

    // Every other element now gets a say, but only to trim: we shape it off to one side and
    // keep however many leading axes still match. The moment inner hits 0 there is nothing
    // left to agree on, so we stop looking (the inner > 0 guard)
    for (uint32_t i = 1; i < v.list.num && inner > 0; i++) {
        uint32_t other[MAX_RANK];
        uint32_t ri = shape_dims(rc_view_value_get(v.list, i), other, max - 1);
        uint32_t common = 0;
        while (common < inner && common < ri && dims[1 + common] == other[common]) {
            common++;
        }
        inner = common;   // the agreed prefix can only shrink, never grow
    }

    return 1 + inner;   // our own axis, plus whatever inner axes survived the haggling
}

// The rank of v: the length of its uniform-length-prefix shape (0 for a scalar).
static uint32_t rank_of(value v)
{
    uint32_t dims[MAX_RANK];
    return shape_dims(v, dims, MAX_RANK);
}

// The shape() function: the axis lengths as a list of numbers (a scalar -> the empty list).
// A function, so it takes the whole argument list and checks it wants exactly one.
static value fn_shape(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;   // functions get raw args now, so propagate an error operand ourselves
    }
    uint32_t dims[MAX_RANK];
    uint32_t rank = shape_dims(v, dims, MAX_RANK);
    rc_array_value out = {0};
    for (uint32_t i = 0; i < rank; i++) {
        rc_array_value_push(&out, value_make_numeric((double)dims[i]), arena);
    }
    return value_make_list(out.view);
}


// ---- the parser ----
// The invariants for one parse, bundled so the recursive helpers stay readable.
typedef struct parser {
    rc_str        text;
    const scopes *scopes;
    uint32_t      scope_index;
    rc_arena     *arena;
} parser;

// Expand a bounded range into its rank-1 list of numeric values; an unbounded range has
// no end to count to, so it cannot be enumerated and yields a domain error instead.
static value range_to_list(value_range r, rc_arena *arena)
{
    if (!r.has_start || !r.has_end) {
        return value_make_error(value_error_domain);
    }
    int64_t step = value_range_step(r);
    rc_array_value out = {0};
    for (int64_t n = r.start; step > 0 ? n <= r.end : n >= r.end; n += step) {
        rc_array_value_push(&out, value_make_numeric((double)n), arena);
    }
    return value_make_list(out.view);
}

// Apply a binary operator, broadcasting component-wise over lists (NumPy-style) by
// recursing into itself - so a ragged tail just broadcasts on its own. Two simple
// operands fall through to the handler (5 + 3 stays 8); errors propagate. A range is just
// a rank-1 list written compactly, so we expand it and broadcast over the elements. The
// shallower operand is a prepended length-1 axis: it is held whole while the deeper one is
// descended. At equal rank a length-1 axis repeats; mismatched lengths are a shape error.
// The result is freshly built; the inputs are only re-read, never copied.
static value apply_binary(lexeme_binary_op op, value a, value b, rc_arena *arena)
{
    if (value_is_error(a)) return a;
    if (value_is_error(b)) return b;
    if (value_is_simple(a) && value_is_simple(b)) {
        return op.apply(a, b, arena);
    }

    // At least one operand is compound; flatten any range to its list so the broadcast
    // below only ever meets lists and scalars.
    if (value_is_range(a)) a = range_to_list(a.range, arena);
    if (value_is_range(b)) b = range_to_list(b.range, arena);
    if (value_is_error(a)) return a;   // an unbounded range could not be enumerated
    if (value_is_error(b)) return b;

    uint32_t ra = rank_of(a);
    uint32_t rb = rank_of(b);
    rc_array_value out = {0};

    if (ra < rb) {   // a is shallower: hold it whole over b's outer axis
        for (uint32_t i = 0; i < b.list.num; i++) {
            rc_array_value_push(&out, apply_binary(op, a, rc_view_value_get(b.list, i), arena), arena);
        }
        return value_make_list(out.view);
    }
    if (rb < ra) {
        for (uint32_t i = 0; i < a.list.num; i++) {
            rc_array_value_push(&out, apply_binary(op, rc_view_value_get(a.list, i), b, arena), arena);
        }
        return value_make_list(out.view);
    }

    // Equal rank, so both are lists: broadcast this axis (a length-1 side repeats).
    uint32_t na = a.list.num;
    uint32_t nb = b.list.num;
    if (na != nb && na != 1 && nb != 1) {
        return value_make_error(value_error_shape_mismatch);
    }
    uint32_t n = (na == 1) ? nb : na;   // (na==1)?nb:na, not max, so 0-length axes work
    for (uint32_t i = 0; i < n; i++) {
        value ai = rc_view_value_get(a.list, na == 1 ? 0 : i);
        value bi = rc_view_value_get(b.list, nb == 1 ? 0 : i);
        rc_array_value_push(&out, apply_binary(op, ai, bi, arena), arena);
    }
    return value_make_list(out.view);
}

// A scalar applies the operator directly; a list maps it element-wise, recursing so nested
// lists map too; a range maps as its enumerated list would; an error short-circuits. The
// new list is built in the (scratch) arena and wrapped - value_make_list does not copy.
static value apply_unary(lexeme_unary_op op, value v, rc_arena *arena)
{
    if (value_is_error(v)) {
        return v;
    }
    if (value_is_list(v)) {
        rc_array_value out = {0};
        for (uint32_t i = 0; i < v.list.num; i++) {
            rc_array_value_push(&out, apply_unary(op, rc_view_value_get(v.list, i), arena), arena);
        }
        return value_make_list(out.view);
    }
    if (value_is_range(v)) {
        // range_to_list carries an unbounded range through as an error, passed straight back.
        return apply_unary(op, range_to_list(v.range, arena), arena);
    }
    return op.apply(v, arena);
}


// ---- subscripting ----
// v[sel0, sel1, ...]: one selector per axis (outermost first), each an integer, a range,
// or a list of integers. An integer drops its axis; a range or list keeps it. Selectors
// compose orthogonally (a cross-product), which falls out of recursing per selected index.

// A selector value as an axis index in [0,len), or RC_INDEX_NONE if it is not a whole
// number in range. Negatives, fractionals and out-of-range all fail: integer and gather
// selectors must land on a real element (only ranges are forgiving, by clamping).
static uint32_t selector_index(value sel, uint32_t len)
{
    if (!value_is_numeric(sel) || sel.numeric < 0.0 || sel.numeric >= (double)len) {
        return RC_INDEX_NONE;
    }
    uint32_t i = (uint32_t)sel.numeric;
    return (double)i == sel.numeric ? i : RC_INDEX_NONE;   // reject a fractional index
}

// The indices a range selector picks along an axis of the given length: the range
// enumerated with unbounded ends standing in for the axis bounds, then kept only where
// they fall in [0,len). Slice semantics - forgiving, never an error (may come back empty).
static rc_array_u32 range_indices(value_range r, uint32_t len, rc_arena *arena)
{
    rc_array_u32 out = {0};
    if (len == 0) {
        return out;
    }

    int64_t step  = value_range_step(r);
    int64_t start = r.has_start ? r.start : (step > 0 ? 0 : (int64_t)len - 1);
    int64_t end   = r.has_end   ? r.end   : (step > 0 ? (int64_t)len - 1 : 0);

    // clamp the far end so the loop cannot run off the axis
    if (step > 0 && end > (int64_t)len - 1) {
        end = (int64_t)len - 1;
    }
    if (step < 0 && end < 0) {
        end = 0;
    }                

    for (int64_t n = start; step > 0 ? n <= end : n >= end; n += step) {
        if (n >= 0 && n < (int64_t)len) {
            rc_array_u32_push(&out, (uint32_t)n, arena);
        }
    }
    return out;
}

// Index a string. A string is rank 1 and always yields a string (there is no character
// type to drop to), so it consumes exactly one selector: an integer gives a 1-char string,
// a range or list the selected characters gathered. Out-of-range integers/elements error.
static value subscript_string(rc_str s, rc_view_value indices, rc_arena *arena)
{
    if (indices.num != 1) {
        return value_make_error(value_error_subscript_range);   // a string takes exactly one axis
    }
    value index = rc_view_value_get(indices, 0);

    if (value_is_numeric(index)) {
        uint32_t i = selector_index(index, s.len);
        if (i == RC_INDEX_NONE) {
            return value_make_error(value_error_subscript_range);
        }
        return value_make_string(rc_str_substr(s, i, 1));
    }

    rc_mstr m = rc_mstr_make(s.len, arena);   // gather the picked characters
    if (value_is_range(index)) {
        rc_array_u32 idx = range_indices(index.range, s.len, arena);
        for (uint32_t j = 0; j < idx.num; j++) {
            rc_mstr_append_char(&m, s.data[RC_AT(idx, j)], arena);
        }
    } else if (value_is_list(index)) {
        for (uint32_t j = 0; j < index.list.num; j++) {
            uint32_t i = selector_index(rc_view_value_get(index.list, j), s.len);
            if (i == RC_INDEX_NONE) {
                return value_make_error(value_error_subscript_range);
            }
            rc_mstr_append_char(&m, s.data[i], arena);
        }
    } else {
        return value_make_error(value_error_type_mismatch);   // selector not int/range/list
    }
    return value_make_string(m.view);
}

// Index a value by indices: the first selector applies to this axis, the rest (a right-
// slice) descend per selected element, so each selector hits one axis and the selectors
// cross-product. An empty selector list returns the value whole (trailing axes untouched);
// a non-subscriptable value errors.
static value subscript(value v, rc_view_value indices, rc_arena *arena)
{
    if (value_is_error(v)) {
        return v;
    }
    if (indices.num == 0) {
        return v;   // no more selectors: this axis and anything within it taken whole
    }
    if (value_is_string(v)) {
        return subscript_string(v.string, indices, arena);
    }
    if (value_is_range(v)) {
        v = range_to_list(v.range, arena);   // index a range via its enumeration
        if (value_is_error(v)) {
            return v;   // an unbounded range cannot be enumerated
        }
    }
    if (!value_is_list(v)) {
        return value_make_error(value_error_type_mismatch);   // a number is not subscriptable
    }

    value sel = rc_view_value_get(indices, 0);
    rc_view_value rest = rc_view_value_get_tail(indices, 1);
    uint32_t len = v.list.num;

    if (value_is_numeric(sel)) {   // an integer drops this axis
        uint32_t i = selector_index(sel, len);
        if (i == RC_INDEX_NONE) {
            return value_make_error(value_error_subscript_range);
        }
        return subscript(rc_view_value_get(v.list, i), rest, arena);
    }

    // a range or list keeps this axis: recurse on each selected element into a result list
    rc_array_value out = {0};
    if (value_is_range(sel)) {
        rc_array_u32 idx = range_indices(sel.range, len, arena);
        for (uint32_t j = 0; j < idx.num; j++) {
            value e = subscript(rc_view_value_get(v.list, RC_AT(idx, j)), rest, arena);
            rc_array_value_push(&out, e, arena);
        }
    } else if (value_is_list(sel)) {
        for (uint32_t j = 0; j < sel.list.num; j++) {
            uint32_t i = selector_index(rc_view_value_get(sel.list, j), len);
            if (i == RC_INDEX_NONE) {
                return value_make_error(value_error_subscript_range);
            }
            value e = subscript(rc_view_value_get(v.list, i), rest, arena);
            rc_array_value_push(&out, e, arena);
        }
    } else {
        return value_make_error(value_error_type_mismatch);   // selector not int/range/list
    }
    return value_make_list(out.view);
}

static expr_result ok(value v, uint32_t next)
{
    return (expr_result) {
        .value = v,
        .next = next,
        .error = expr_error_none
    };
}

static expr_result fail(expr_error error, uint32_t at)
{
    return (expr_result) {
        .next = at,
        .error = error,
        .error_at = at
    };
}

// ---- list, reduction and query functions ----

// Fold elems left-to-right with op, broadcasting at each step (so a list of vectors reduces
// element-wise). identity seeds the accumulator; pass value_make_none() for an op with no
// identity (min/max), which then seeds with the first element and errors on an empty list.
static value fold(rc_view_value elems, value (*op)(value, value, rc_arena *), value identity, rc_arena *arena)
{
    lexeme_binary_op binop = {.apply = op};
    bool has_identity = !value_is_none(identity);
    if (!has_identity && elems.num == 0) {
        return value_make_error(value_error_domain);   // an empty reduction with no identity
    }
    value acc = has_identity ? identity : rc_view_value_get(elems, 0);
    for (uint32_t i = has_identity ? 0 : 1; i < elems.num; i++) {
        acc = apply_binary(binop, acc, rc_view_value_get(elems, i), arena);
    }
    return acc;
}

// Reduce v over a single axis: axis 0 folds the elements together (collapsing the outer
// axis); a deeper axis recurses into each element. The axis is assumed in range.
static value reduce_axis(value v, uint32_t axis, value (*op)(value, value, rc_arena *), value identity, rc_arena *arena)
{
    if (!value_is_list(v)) {
        return value_make_error(value_error_subscript_range);   // axis out of range for this branch
    }
    if (axis == 0) {
        return fold(v.list, op, identity, arena);
    }
    rc_array_value out = {0};
    for (uint32_t i = 0; i < v.list.num; i++) {
        value r = reduce_axis(rc_view_value_get(v.list, i), axis - 1, op, identity, arena);
        rc_array_value_push(&out, r, arena);
    }
    return value_make_list(out.view);
}

// Collect every leaf of v (descending lists and, by enumeration, ranges) into out.
static void flatten_into(value v, rc_array_value *out, rc_arena *arena)
{
    if (value_is_list(v)) {
        for (uint32_t i = 0; i < v.list.num; i++) {
            flatten_into(rc_view_value_get(v.list, i), out, arena);
        }
    } else if (value_is_range(v)) {
        flatten_into(range_to_list(v.range, arena), out, arena);
    } else {
        rc_array_value_push(out, v, arena);
    }
}

// The shared reduction body: f(L) folds every leaf to a scalar; f(L, axis) collapses one
// axis. op + identity pick the specific reduction (sum / product / min / max).
static value reduce(rc_view_value args, value (*op)(value, value, rc_arena *), value identity, rc_arena *arena)
{
    if (args.num < 1 || args.num > 2) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    if (value_is_range(v)) {
        v = range_to_list(v.range, arena);
        if (value_is_error(v)) return v;
    }

    if (args.num == 1) {
        rc_array_value leaves = {0};
        flatten_into(v, &leaves, arena);
        return fold(leaves.view, op, identity, arena);
    }

    uint32_t axis = selector_index(rc_view_value_get(args, 1), rank_of(v));   // 0 <= axis < rank
    if (axis == RC_INDEX_NONE) {
        return value_make_error(value_error_subscript_range);
    }
    return reduce_axis(v, axis, op, identity, arena);
}

static value fn_sum(rc_view_value args, rc_arena *arena)     { return reduce(args, op_add, value_make_numeric(0), arena); }
static value fn_product(rc_view_value args, rc_arena *arena) { return reduce(args, op_mul, value_make_numeric(1), arena); }
static value fn_min(rc_view_value args, rc_arena *arena)     { return reduce(args, op_min, value_make_none(), arena); }
static value fn_max(rc_view_value args, rc_arena *arena)     { return reduce(args, op_max, value_make_none(), arena); }

// len: the length of the outermost axis (list elements, string characters, range count).
static value fn_len(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    if (value_is_string(v)) return value_make_numeric(v.string.len);
    if (value_is_list(v))   return value_make_numeric(v.list.num);
    if (value_is_range(v)) {
        value l = range_to_list(v.range, arena);
        return value_is_error(l) ? l : value_make_numeric(l.list.num);
    }
    return value_make_error(value_error_type_mismatch);   // a scalar has no length
}

// rank: the number of axes (0 for a scalar).
static value fn_rank(rc_view_value args, rc_arena *arena)
{
    (void)arena;
    if (args.num != 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    return value_is_error(v) ? v : value_make_numeric(rank_of(v));
}

// flatten: every leaf, in order, as a single rank-1 list.
static value fn_flatten(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    rc_array_value out = {0};
    flatten_into(v, &out, arena);
    return value_make_list(out.view);
}

// concat: join the arguments along axis 0 - each list (or range) contributes its elements,
// each scalar joins as a single element.
static value fn_concat(rc_view_value args, rc_arena *arena)
{
    rc_array_value out = {0};
    for (uint32_t i = 0; i < args.num; i++) {
        value a = rc_view_value_get(args, i);
        if (value_is_error(a)) {
            return a;
        }
        if (value_is_range(a)) {
            a = range_to_list(a.range, arena);
            if (value_is_error(a)) return a;
        }
        if (value_is_list(a)) {
            for (uint32_t j = 0; j < a.list.num; j++) {
                rc_array_value_push(&out, rc_view_value_get(a.list, j), arena);
            }
        } else {
            rc_array_value_push(&out, a, arena);
        }
    }
    return value_make_list(out.view);
}

// zip: turn N equal-length lists into one list of N-tuples (the transpose of the stacked
// arguments), so zip({1,2,3},{4,5,6}) == {{1,4},{2,5},{3,6}}. Ranges coerce to lists; the
// arguments must all share the same outer length.
static value fn_zip(rc_view_value args, rc_arena *arena)
{
    rc_array_value lists = {0};   // the arguments, each coerced to a list
    uint32_t n = 0;
    for (uint32_t i = 0; i < args.num; i++) {
        value a = rc_view_value_get(args, i);
        if (value_is_error(a)) {
            return a;
        }
        if (value_is_range(a)) {
            a = range_to_list(a.range, arena);
            if (value_is_error(a)) return a;
        }
        if (!value_is_list(a)) {
            return value_make_error(value_error_type_mismatch);
        }
        if (i == 0) {
            n = a.list.num;
        }
        else if (a.list.num != n) {
            return value_make_error(value_error_shape_mismatch);   // lengths must match
        }
        rc_array_value_push(&lists, a, arena);
    }

    rc_array_value out = {0};
    for (uint32_t i = 0; i < n; i++) {
        rc_array_value tuple = {0};
        for (uint32_t j = 0; j < lists.num; j++) {
            value lst = rc_view_value_get(lists.view, j);
            rc_array_value_push(&tuple, rc_view_value_get(lst.list, i), arena);
        }
        rc_array_value_push(&out, value_make_list(tuple.view), arena);
    }
    return value_make_list(out.view);
}

// reverse: the outermost axis reversed (a list's elements, or a string's characters).
static value fn_reverse(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    if (value_is_range(v)) {
        v = range_to_list(v.range, arena);
        if (value_is_error(v)) return v;
    }
    if (value_is_string(v)) {
        rc_mstr m = rc_mstr_make(v.string.len, arena);
        for (uint32_t i = v.string.len; i-- > 0; ) {
            rc_mstr_append_char(&m, v.string.data[i], arena);
        }
        return value_make_string(m.view);
    }
    if (value_is_list(v)) {
        // Copy first (the source view may alias shared storage), then reverse in place.
        rc_array_value out = rc_array_value_make_copy(v.list, v.list.num, arena);
        rc_span_value_reverse(out.span);
        return value_make_list(out.view);
    }
    return value_make_error(value_error_type_mismatch);
}

// sort: order a list's elements ascending by a numeric key. The key is the element itself,
// or - given extra arguments - the result of subscripting each element by them, so
// sort(L, 0) sorts on each element's first item. The key must resolve to a number. We pair
// each element with its key once, then sort the pairs (richc introsort) on the stored key.
typedef struct sort_pair {
    double key;
    value  v;
} sort_pair;

#define RC_ARRAY_TYPE sort_pair
#include "richc/template/array.h"

#define RC_SORT_TYPE sort_pair
#define RC_SORT_NAME sort_pairs
#define RC_SORT_CMP(a, b) ((a).key < (b).key)
#include "richc/template/algorithm/sort.h"

static value fn_sort(rc_view_value args, rc_arena *arena)
{
    if (args.num < 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    if (value_is_range(v)) {
        v = range_to_list(v.range, arena);
        if (value_is_error(v)) return v;
    }
    if (!value_is_list(v)) {
        return value_make_error(value_error_type_mismatch);
    }

    rc_view_value key_path = rc_view_value_get_tail(args, 1);   // the per-element subscript to the key
    uint32_t n = v.list.num;
    rc_array_sort_pair pairs = rc_array_sort_pair_make(n, arena);
    for (uint32_t i = 0; i < n; i++) {
        value e = rc_view_value_get(v.list, i);
        value key = subscript(e, key_path, arena);   // an empty path leaves the element itself
        if (value_is_error(key)) {
            return key;
        }
        if (!value_is_numeric(key)) {
            return value_make_error(value_error_type_mismatch);   // the key must be a number
        }
        rc_array_sort_pair_push(&pairs, (sort_pair) {.key = key.numeric, .v = e}, arena);
    }
    sort_pairs(pairs.span);

    rc_array_value out = {0};
    for (uint32_t i = 0; i < n; i++) {
        rc_array_value_push(&out, RC_AT(pairs, i).v, arena);
    }
    return value_make_list(out.view);
}

// defined: false only for an unresolved symbol; true for any other value (including other
// errors). Unlike the rest, it inspects its argument's error rather than propagating it -
// which is the whole point, so it works on a symbol that has not been defined yet.
static value fn_defined(rc_view_value args, rc_arena *arena)
{
    (void)arena;
    if (args.num != 1) {
        return value_make_error(value_error_incorrect_parameters);
    }
    value v = rc_view_value_get(args, 0);
    bool unresolved = value_is_error(v) && v.error == value_error_unknown_symbol;
    return value_make_numeric(unresolved ? 0.0 : 1.0);
}


// ---- the two context tables ----
// EVEN: lexed where an operand is expected (the start, after a binary op, after an
// open paren). Numbers/strings/identifiers come from the lexer itself, so the table
// only carries the leading operators, the functions, and the open paren.
static const token even_entries[] = {
    {RC_STR("("),     {.type = lexeme_type_open_paren}},
    {RC_STR("{"),     {.type = lexeme_type_open_brace}},            // begins a list literal
    {RC_STR("}"),     {.type = lexeme_type_close_brace}},           // ends one (empty, or after a comma)

    {RC_STR(".."),    {.type = lexeme_type_range}},                 // start-unbounded range
    {RC_STR("..<"),   {.type = lexeme_type_range, .range = {.exclusive = true}}},

    {RC_STR("+"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = op_pos, .precedence = prec_neg}}},
    {RC_STR("-"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = op_neg, .precedence = prec_neg}}},
    // Bare low/high-byte operators (6502 style): '<' is the low byte, '>' the high byte. Very
    // low precedence, so they swallow the whole following expression: <start+1 is lo(start+1).
    {RC_STR("<"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_lo, .precedence = prec_lohi}}},
    {RC_STR(">"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_hi, .precedence = prec_lohi}}},

    // Element-wise builtins are parenthesised unary ops: the '(' is part of the token (so
    // the name only reads as a call when followed by '(' - 'lo' is a variable, 'lo(' the op),
    // and precedence is unused (left default) because the argument is closed by ')'.
    {RC_STR("abs("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_abs}}},
    {RC_STR("lo("),    {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_lo}}},
    {RC_STR("hi("),    {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_hi}}},
    {RC_STR("sqrt("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_sqrt}}},
    {RC_STR("not("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_not}}},
    {RC_STR("int("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_int}}},
    {RC_STR("floor("), {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_int}}},
    {RC_STR("round("), {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_round}}},
    {RC_STR("ceil("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_ceil}}},
    {RC_STR("sin("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_sin}}},
    {RC_STR("cos("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_cos}}},
    {RC_STR("tan("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_tan}}},
    {RC_STR("asin("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_asin}}},
    {RC_STR("acos("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_acos}}},
    {RC_STR("atan("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_atan}}},
    {RC_STR("log("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_log}}},     // base 10
    {RC_STR("ln("),    {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_ln}}},      // natural
    {RC_STR("exp("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_exp}}},

    // Structural/variadic builtins are functions: the handler gets the whole arg list.
    {RC_STR("shape("),   {.type = lexeme_type_function, .function = {.apply = fn_shape}}},
    {RC_STR("len("),     {.type = lexeme_type_function, .function = {.apply = fn_len}}},
    {RC_STR("rank("),    {.type = lexeme_type_function, .function = {.apply = fn_rank}}},
    {RC_STR("flatten("), {.type = lexeme_type_function, .function = {.apply = fn_flatten}}},
    {RC_STR("concat("),  {.type = lexeme_type_function, .function = {.apply = fn_concat}}},
    {RC_STR("zip("),      {.type = lexeme_type_function, .function = {.apply = fn_zip}}},
    {RC_STR("reverse("), {.type = lexeme_type_function, .function = {.apply = fn_reverse}}},
    {RC_STR("sort("),    {.type = lexeme_type_function, .function = {.apply = fn_sort}}},
    {RC_STR("sum("),     {.type = lexeme_type_function, .function = {.apply = fn_sum}}},
    {RC_STR("product("), {.type = lexeme_type_function, .function = {.apply = fn_product}}},
    {RC_STR("min("),     {.type = lexeme_type_function, .function = {.apply = fn_min}}},
    {RC_STR("max("),     {.type = lexeme_type_function, .function = {.apply = fn_max}}},
    {RC_STR("defined("), {.type = lexeme_type_function, .function = {.apply = fn_defined}}},
};

// ODD: lexed where a binary operator is expected (after an operand). The close paren
// lives here, so a parenthesised group is closed from operator position.
static const token odd_entries[] = {
    {RC_STR(")"),   {.type = lexeme_type_close_paren}},
    {RC_STR("}"),   {.type = lexeme_type_close_brace}},         // ends a list (after an element)
    {RC_STR("["),   {.type = lexeme_type_open_bracket}},        // postfix subscript
    {RC_STR("]"),   {.type = lexeme_type_close_bracket}},       // ends a subscript

    {RC_STR(".."),  {.type = lexeme_type_range}},               // range operator (handled specially)
    {RC_STR("..<"), {.type = lexeme_type_range, .range = {.exclusive = true}}},

    {RC_STR("^"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_pow,  .precedence = prec_pow, .associativity = assoc_right}}},
    {RC_STR("*"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_mul,  .precedence = prec_mul}}},
    {RC_STR("/"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_div,  .precedence = prec_mul}}},
    {RC_STR("div"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_idiv, .precedence = prec_mul}}},
    {RC_STR("mod"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_mod,  .precedence = prec_mul}}},
    {RC_STR("<<"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_shl,  .precedence = prec_mul}}},
    {RC_STR(">>"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_shr,  .precedence = prec_mul}}},
    {RC_STR("+"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_add,  .precedence = prec_add}}},
    {RC_STR("-"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_sub,  .precedence = prec_add}}},
    {RC_STR("="),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_eq,   .precedence = prec_cmp}}},
    {RC_STR("=="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_eq,   .precedence = prec_cmp}}},
    {RC_STR("!="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_ne,   .precedence = prec_cmp}}},
    {RC_STR("<>"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_ne,   .precedence = prec_cmp}}},
    {RC_STR("<="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_le,   .precedence = prec_cmp}}},
    {RC_STR(">="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_ge,   .precedence = prec_cmp}}},
    {RC_STR("<"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_lt,   .precedence = prec_cmp}}},
    {RC_STR(">"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_gt,   .precedence = prec_cmp}}},
    {RC_STR("and"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_and,  .precedence = prec_and}}},
    {RC_STR("or"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_or,   .precedence = prec_or}}},
    {RC_STR("eor"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_eor,  .precedence = prec_or}}},
};

static const token_table even_tokens = RC_VIEW(even_entries);
static const token_table odd_tokens  = RC_VIEW(odd_entries);


// Require a ')' at cursor (lexed from operator position). On success returns v with
// the cursor past the ')'; otherwise an expected_close_paren error.
static expr_result expect_close_paren(const parser *p, value v, uint32_t cursor)
{
    lexer_result rp = lexer_next(p->text, cursor, odd_tokens);
    if (rp.token.type != lexeme_type_close_paren) {
        return fail(expr_error_expected_close_paren, cursor);
    }
    return ok(v, rp.next);
}

// parse_operand and parse_precedence are mutually recursive, so one of the pair must
// be declared ahead; everything else below is defined in call order (callees first).
static expr_result parse_precedence(const parser *p, uint32_t cursor, uint8_t min_prec);

// Accept a newline if one is here (a list literal treats newlines as whitespace),
// returning the cursor past it, else the cursor unchanged. The lexer coalesces a run
// of newlines into one terminator, so there is only ever one to skip. We lex with `tt`
// so the caller can re-lex the same spot for whatever it expects there; a ':' or EOF
// lexes as a hard terminator and is left in place, so an unclosed list is reported.
static uint32_t accept_newline(const parser *p, uint32_t cursor, token_table tt)
{
    lexer_result lr = lexer_next(p->text, cursor, tt);
    if (lr.token.type == lexeme_type_terminator && lr.token.terminator.newline) {
        return lr.next;
    }
    return cursor;
}

// Parse a list literal from just after the '{': comma-separated element expressions
// (nested lists allowed, empty allowed), with newlines ignored inside the braces. We
// gather the elements in the parser's scratch arena and wrap that view - no copy,
// since a value is a non-owning handle.
static expr_result parse_list(const parser *p, uint32_t cursor)
{
    rc_array_value elems = {0};

    while (true) {
        // Value-or-'}' position.
        cursor = accept_newline(p, cursor, even_tokens);
        lexer_result lr = lexer_next(p->text, cursor, even_tokens);
        if (lr.token.type == lexeme_type_close_brace) {
            return ok(value_make_list(elems.view), lr.next);   // possibly empty
        }

        // One element, a full expression. A soft fail here (e.g. "{,}") is a real error.
        expr_result e = parse_precedence(p, cursor, 0);
        if (e.error != expr_error_none) {
            return e;
        }
        rc_array_value_push(&elems, e.value, p->arena);
        cursor = e.next;

        // Separator position: after any newline we want a ',' or '}'.
        cursor = accept_newline(p, cursor, odd_tokens);
        lr = lexer_next(p->text, cursor, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            cursor = lr.next;
            continue;   // a trailing comma simply loops back and finds the '}'
        }
        if (lr.token.type == lexeme_type_close_brace) {
            return ok(value_make_list(elems.view), lr.next);
        }
        return fail(expr_error_expected_close_brace, cursor);
    }
}

// Parse a function call's arguments from just after the '(' (which was part of the token):
// comma-separated expressions up to the ')', then hand the raw arguments to the function's
// handler, which validates the count and types and decides how to treat errors (most
// propagate, defined() inspects). An empty list is allowed; a missing ')' is committed.
static expr_result parse_call_args(const parser *p, lexeme_function fn, uint32_t cursor)
{
    RC_ASSERT(cursor > 0 && p->text.data[cursor - 1] == '(');   // the '(' is part of the function token

    rc_array_value args = {0};

    // An immediate ')' is an empty argument list.
    lexer_result lr = lexer_next(p->text, cursor, odd_tokens);
    if (lr.token.type == lexeme_type_close_paren) {
        return ok(fn.apply(args.view, p->arena), lr.next);
    }

    while (true) {
        expr_result a = parse_precedence(p, cursor, 0);
        if (a.error != expr_error_none) {
            return a;   // a soft expected_expression here (e.g. "f(1,)") is a real error
        }
        rc_array_value_push(&args, a.value, p->arena);
        cursor = a.next;

        lr = lexer_next(p->text, cursor, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            cursor = lr.next;
            continue;
        }
        if (lr.token.type == lexeme_type_close_paren) {
            return ok(fn.apply(args.view, p->arena), lr.next);
        }
        return fail(expr_error_expected_close_paren, cursor);
    }
}

// Parse one operand: a literal, a symbol, a parenthesised group, a prefixed unary
// expression, a function call, or a list literal. A lexeme that cannot begin an
// operand is a soft expected_expression failure, leaving the caller to decide.
static expr_result parse_operand(const parser *p, uint32_t cursor)
{
    lexer_result lr = lexer_next(p->text, cursor, even_tokens);
    lexeme lex = lr.token;

    switch (lex.type) {
        case lexeme_type_numeric_literal:
            return ok(value_make_numeric(lex.numeric_literal.value), lr.next);

        case lexeme_type_string_literal:
            // Plain literal: wrap the source view directly, no copy.
            return ok(value_make_string(lex.string_literal.ref), lr.next);

        case lexeme_type_escaped_string_literal: {
            // The ref still holds each escaped quote as a doubled "", so copy it into the
            // scratch arena and collapse every pair to a single quote.
            rc_mstr m = rc_mstr_from_str(lex.string_literal.ref, lex.string_literal.ref.len, p->arena);
            rc_mstr_replace(&m, RC_STR("\"\""), RC_STR("\""), p->arena);
            return ok(value_make_string(m.view), lr.next);
        }

        case lexeme_type_identifier: {
            value v = scopes_get_symbol(p->scopes, p->scope_index, lex.identifier.name);
            // Not found is not a parse error: it becomes an error value that
            // propagates, so a forward reference can resolve on a later pass.
            return ok(value_is_none(v) ? value_make_error(value_error_unknown_symbol) : v, lr.next);
        }

        case lexeme_type_open_paren: {
            expr_result sub = parse_precedence(p, lr.next, 0);
            if (sub.error != expr_error_none) {
                return sub;   // a soft fail here (e.g. "()") is a genuine error in parens
            }
            
            return expect_close_paren(p, sub.value, sub.next);
        }

        case lexeme_type_open_brace:
            return parse_list(p, lr.next);

        case lexeme_type_unary_op: {
            // A '(' baked into the token name (abs(, sqrt(, ...) marks the parenthesised form:
            // the argument is then a whole expression closed by ')'. Otherwise (- and +) the
            // operand is parsed at the operator's own precedence (prefix climb).
            if (p->text.data[lr.next - 1] == '(') {
                expr_result arg = parse_precedence(p, lr.next, 0);

                if (arg.error != expr_error_none) {
                    return arg;   // missing/garbled argument: malformed
                }

                return expect_close_paren(p, apply_unary(lex.unary_op, arg.value, p->arena), arg.next);
            }

            expr_result arg = parse_precedence(p, lr.next, lex.unary_op.precedence);
            if (arg.error != expr_error_none) {
                return arg;   // nothing to apply the operator to: soft fail bubbles up
            }

            return ok(apply_unary(lex.unary_op, arg.value, p->arena), arg.next);
        }

        case lexeme_type_function:
            // The '(' is part of the token name, so we are already inside the call.
            return parse_call_args(p, lex.function, lr.next);

        case lexeme_type_range: {
            // A '..' where an operand is expected opens an unbounded-start range. Parse a
            // single endpoint (above range precedence, so '..b..c' leaves the second '..'
            // to error in the caller); nothing there gives the fully-open '..'.
            expr_result end = parse_precedence(p, lr.next, prec_range + 1);
            if (end.error == expr_error_expected_expression) {
                return ok(value_make_range_open(lex.range.exclusive), lr.next);
            }

            if (end.error != expr_error_none) {
                return end;
            }

            return ok(value_make_range_open_start(end.value, lex.range.exclusive), end.next);
        }

        default:
            // A terminator, a stray close paren, a lexer error: no operand here.
            return fail(expr_error_expected_expression, cursor);
    }
}

// Parse a subscript from just after the '[': comma-separated selector expressions up to
// the ']', then index target by them. Selectors are full expressions (so i, a..b, {..}
// and a bare .. all work, each stopping cleanly at the ',' or ']'). Newlines are not
// skipped - a subscript is an inline postfix. An empty '[]', a trailing comma, or a
// missing ']' is a committed error; index/type problems become propagating error values.
static expr_result parse_subscript(const parser *p, value target, uint32_t cursor)
{
    rc_array_value indices = {0};

    while (true) {
        expr_result s = parse_precedence(p, cursor, 0);
        if (s.error == expr_error_expected_expression) {
            return fail(expr_error_expected_expression, cursor);   // empty "[]" or a trailing comma
        }

        if (s.error != expr_error_none) {
            return s;
        }

        rc_array_value_push(&indices, s.value, p->arena);
        cursor = s.next;

        lexer_result lr = lexer_next(p->text, cursor, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            cursor = lr.next;
            continue;
        }

        if (lr.token.type == lexeme_type_close_bracket) {
            return ok(subscript(target, indices.view, p->arena), lr.next);
        }

        return fail(expr_error_expected_close_bracket, cursor);
    }
}

// Parse an expression whose operators bind at least as tightly as min_prec, folding
// left-to-right. Stops (greedily) at the first lexeme that is not a usable binary
// operator, returning the value so far and the cursor before that lexeme.
static expr_result parse_precedence(const parser *p, uint32_t cursor, uint8_t min_prec)
{
    expr_result lhs = parse_operand(p, cursor);
    if (lhs.error != expr_error_none) {
        return lhs;   // could not even get the first operand
    }

    while (true) {
        lexer_result lr = lexer_next(p->text, lhs.next, odd_tokens);

        switch (lr.token.type) {
            // A normal infix operator: parse its right side at the precedence its
            // associativity dictates, then broadcast the two through apply_binary.
            case lexeme_type_binary_op: {
                lexeme_binary_op op = lr.token.binary_op;
                if (op.precedence < min_prec) {
                    return lhs;   // binds looser than the caller allows: leave it to them
                }
                // Left-assoc parses its right side one notch higher so a-b-c folds as
                // (a-b)-c; right-assoc keeps the level so a^b^c folds as a^(b^c).
                uint8_t next_min = (uint8_t) (op.precedence + (op.associativity == assoc_left ? 1 : 0));
                expr_result rhs = parse_precedence(p, lr.next, next_min);

                if (rhs.error == expr_error_expected_expression) {
                    return lhs;   // a trailing operator with no operand ("1+3+"): roll it back
                }
                if (rhs.error != expr_error_none) {
                    return rhs;   // a real error on the right-hand side: propagate
                }
                lhs.value = apply_binary(op, lhs.value, rhs.value, p->arena);
                lhs.next  = rhs.next;
                break;
            }

            // The range operator '..' / '..<'. It is the loosest operator and right-
            // associative, so a..b..c folds as a..(b..c) and a range on the right is the
            // stepped form. It builds from its raw operands (no broadcast, no coercion),
            // and a missing right side is not an error but an unbounded end ("a..").
            case lexeme_type_range: {
                if (prec_range < min_prec) {
                    return lhs;
                }
                expr_result rhs = parse_precedence(p, lr.next, prec_range);   // right-assoc: same level

                if (rhs.error == expr_error_expected_expression) {
                    lhs.value = value_make_range_open_end(lhs.value, lr.token.range.exclusive);
                    lhs.next  = lr.next;
                    break;
                }
                if (rhs.error != expr_error_none) {
                    return rhs;
                }
                lhs.value = lr.token.range.exclusive
                    ? op_range_excl(lhs.value, rhs.value, p->arena)
                    : op_range(lhs.value, rhs.value, p->arena);
                lhs.next  = rhs.next;
                break;
            }

            // A postfix subscript. It binds tightest, so it always applies to the operand
            // just parsed; looping back lets L[..][..] chain.
            case lexeme_type_open_bracket: {
                if (prec_subscript < min_prec) {
                    return lhs;
                }
                expr_result sub = parse_subscript(p, lhs.value, lr.next);
                if (sub.error != expr_error_none) {
                    return sub;
                }
                lhs = sub;
                break;
            }

            default:
                return lhs;   // not a usable infix operator: stop, leaving it unconsumed
        }
    }
}

expr_result expression_parse(rc_str text, uint32_t cursor,
                             const scopes *s, uint32_t scope_index, rc_arena *arena)
{
    RC_ASSERT(rc_str_is_valid(text));
    RC_ASSERT(s != NULL);

    parser p = {
        .text = text,
        .scopes = s,
        .scope_index = scope_index,
        .arena = arena
    };

    return parse_precedence(&p, cursor, 0);
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Every test parses against a fresh root scope and a scratch arena. The fixture
// stands both up before each test and tears them down after, so the tests
// themselves are nothing but the expressions and their expected results.
RC_TEST_GROUP_DATA(expression) {
    scopes   scopes;
    rc_arena arena;
};

RC_TEST_GROUP_INIT(expression, fix)
{
    fix->arena = rc_arena_make_default();
    scopes_init(&fix->scopes);
    scopes_make_root(&fix->scopes);
}

RC_TEST_GROUP_DEINIT(expression, fix)
{
    scopes_deinit(&fix->scopes);
    rc_arena_deinit(&fix->arena);
}

// Parse a source literal from offset 0 in the fixture's root scope.
#define RESULT(src) expression_parse(RC_STR(src), 0, &fix->scopes, 0, &fix->arena)
#define VAL(src)    RESULT(src).value

// A bounded range with the expected start/end/step.
static bool range_is(value v, int64_t start, int64_t end, int64_t step)
{
    return value_is_range(v) && v.range.has_start && v.range.has_end
        && v.range.start == start && v.range.end == end && v.range.step == step;
}

RC_TEST_STEP(expression, arithmetic_and_precedence, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("1+2*3"),   value_make_numeric(7.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("10-2-3"),  value_make_numeric(5.0)));    // left assoc
    RC_CHECK_TRUE(value_is_equal(VAL("2^3^2"),   value_make_numeric(512.0)));  // right assoc
    RC_CHECK_TRUE(value_is_equal(VAL("(1+2)*3"), value_make_numeric(9.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("7/2"),     value_make_numeric(3.5)));
}

RC_TEST_STEP(expression, unary, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("-2^2"), value_make_numeric(-4.0)));   // -(2^2)
    RC_CHECK_TRUE(value_is_equal(VAL("-2*3"), value_make_numeric(-6.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("+5"),   value_make_numeric(5.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("--5"),  value_make_numeric(5.0)));    // -(-5)
}

RC_TEST_STEP(expression, functions, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("ABS(-5)"),  value_make_numeric(5.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("ABS(3-7)"), value_make_numeric(4.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("abs(-1)"),  value_make_numeric(1.0)));   // case-insensitive
}

RC_TEST_STEP(expression, callable_syntax, fix)
{
    // A named callable needs its '(': a bare name is now a plain identifier, so a function
    // and a like-named variable can coexist.
    scopes_set_symbol(&fix->scopes, 0, RC_STR("lo"), value_make_numeric(7.0), (source_pos){0, 0});
    RC_CHECK_TRUE(value_is_equal(VAL("lo"),      value_make_numeric(7.0)));   // the variable
    RC_CHECK_TRUE(value_is_equal(VAL("lo(258)"), value_make_numeric(2.0)));   // the operator (low byte)
    RC_CHECK_TRUE(value_is_error(VAL("abs")));   // a bare function name is just an unknown symbol

    // a parenthesised unary op still maps element-wise over a compound value
    value r12[] = {value_make_numeric(1), value_make_numeric(2)};
    RC_CHECK_TRUE(value_is_equal(VAL("abs({-1,-2})"), value_make_list((rc_view_value) RC_VIEW(r12))));

    // ... and takes exactly one argument
    RC_CHECK_TRUE(RESULT("abs()").error    != expr_error_none);   // no argument
    RC_CHECK_TRUE(RESULT("abs(1,2)").error != expr_error_none);   // an extra argument

    // a variadic function validates its own arity
    RC_CHECK_TRUE(value_is_error(VAL("shape(1,2)")));   // too many args -> error value
    RC_CHECK_TRUE(value_is_error(VAL("shape()")));      // too few args
    value sh[] = {value_make_numeric(2), value_make_numeric(2)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape({{1,2},{3,4}})"), value_make_list((rc_view_value) RC_VIEW(sh))));
}

RC_TEST_STEP(expression, symbols, fix)
{
    scopes_set_symbol(&fix->scopes, 0, RC_STR("foo"), value_make_numeric(42.0), (source_pos){0, 0});

    RC_CHECK_TRUE(value_is_equal(VAL("foo+1"), value_make_numeric(43.0)));
    RC_CHECK_TRUE(value_is_error(VAL("bar")));        // unknown symbol -> error value
    RC_CHECK_TRUE(value_is_error(VAL("bar+1")));      // and it propagates through arithmetic
}

RC_TEST_STEP(expression, eval_errors, fix)
{
    RC_CHECK_TRUE(value_is_error(VAL("1/0")));        // divide by zero
}

RC_TEST_STEP(expression, greedy_stop, fix)
{
    // A close paren we did not open is left for the caller: stop after the '2'.
    expr_result a = RESULT("1+2)");
    RC_CHECK_TRUE(value_is_equal(a.value, value_make_numeric(3.0)));
    RC_CHECK_TRUE(a.error == expr_error_none);
    RC_CHECK(a.next, ==, 3u);

    // A trailing binary op with no operand is rolled back: stop after the '3'.
    expr_result b = RESULT("1+3+");
    RC_CHECK_TRUE(value_is_equal(b.value, value_make_numeric(4.0)));
    RC_CHECK_TRUE(b.error == expr_error_none);
    RC_CHECK(b.next, ==, 3u);
}

RC_TEST_STEP(expression, committed_errors, fix)
{
    RC_CHECK_TRUE(RESULT("(1+2").error    == expr_error_expected_close_paren);
    RC_CHECK_TRUE(RESULT("ABS(3").error   == expr_error_expected_close_paren);   // unary op missing ')'
    RC_CHECK_TRUE(RESULT("shape(1").error == expr_error_expected_close_paren);   // function missing ')'
    RC_CHECK_TRUE(RESULT(")").error       == expr_error_expected_expression);
}

RC_TEST_STEP(expression, shifts, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("1<<4"),   value_make_numeric(16.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("256>>2"), value_make_numeric(64.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("1<<2+1"), value_make_numeric(5.0)));   // (1<<2)+1, shift binds like *
}

RC_TEST_STEP(expression, integer_div_mod, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("7 div 2"),  value_make_numeric(3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("-7 div 2"), value_make_numeric(-3.0)));   // toward zero
    RC_CHECK_TRUE(value_is_equal(VAL("7 mod 2"),  value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("-7 mod 2"), value_make_numeric(-1.0)));   // sign of dividend
    RC_CHECK_TRUE(value_is_error(VAL("5 div 0")));
}

RC_TEST_STEP(expression, bitwise, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("12 and 10"), value_make_numeric(8.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("12 or 3"),   value_make_numeric(15.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("12 eor 10"), value_make_numeric(6.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("NOT(0)"),    value_make_numeric(4294967295.0)));   // unsigned ~0
    RC_CHECK_TRUE(value_is_equal(VAL("NOT(255)"),  value_make_numeric(4294967040.0)));
}

RC_TEST_STEP(expression, comparisons, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("3=3"),   value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("3=4"),   value_make_numeric(0.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("3==3"),  value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("3!=4"),  value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("3<>3"),  value_make_numeric(0.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("2<3"),   value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("3<=3"),  value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("4>=5"),  value_make_numeric(0.0)));
    // arithmetic binds tighter than comparison, comparison tighter than and/or.
    RC_CHECK_TRUE(value_is_equal(VAL("1+1=2"),         value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("2<3 and 4<5"),   value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("0 or 1"),        value_make_numeric(1.0)));
}

RC_TEST_STEP(expression, more_functions, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("LO(258)"),   value_make_numeric(2.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("HI(258)"),   value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("SQRT(9)"),   value_make_numeric(3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("INT(2.7)"),  value_make_numeric(2.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("INT(-2.7)"), value_make_numeric(-3.0)));   // floor, toward -inf
    RC_CHECK_TRUE(value_is_equal(VAL("FLOOR(-2.7)"), value_make_numeric(-3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("ROUND(2.7)"),  value_make_numeric(2.0)));  // trunc, toward zero
    RC_CHECK_TRUE(value_is_equal(VAL("ROUND(-2.7)"), value_make_numeric(-2.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("CEIL(2.1)"),   value_make_numeric(3.0)));  // toward +inf
    RC_CHECK_TRUE(value_is_error(VAL("SQRT(-1)")));
}

RC_TEST_STEP(expression, list_literals, fix)
{
    value empty = VAL("{}");
    RC_CHECK_TRUE(value_is_list(empty));
    RC_CHECK(empty.list.num, ==, 0u);

    value e123[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3)};
    RC_CHECK_TRUE(value_is_equal(VAL("{1,2,3}"), value_make_list((rc_view_value) RC_VIEW(e123))));

    // Elements are full expressions.
    value e312[] = {value_make_numeric(3), value_make_numeric(12)};
    RC_CHECK_TRUE(value_is_equal(VAL("{1+2,3*4}"), value_make_list((rc_view_value) RC_VIEW(e312))));

    // Nested lists.
    value inner[] = {value_make_numeric(2), value_make_numeric(3)};
    value nested[] = {value_make_numeric(1), value_make_list((rc_view_value) RC_VIEW(inner)), value_make_numeric(4)};
    RC_CHECK_TRUE(value_is_equal(VAL("{1,{2,3},4}"), value_make_list((rc_view_value) RC_VIEW(nested))));
}

RC_TEST_STEP(expression, list_newlines_and_commas, fix)
{
    value e12[] = {value_make_numeric(1), value_make_numeric(2)};
    value want12 = value_make_list((rc_view_value) RC_VIEW(e12));

    RC_CHECK_TRUE(value_is_equal(VAL("{1,\n2}"),     want12));   // newline after a comma
    RC_CHECK_TRUE(value_is_equal(VAL("{\n1,\n2\n}"), want12));   // newlines throughout
    RC_CHECK_TRUE(value_is_equal(VAL("{1,2,}"),      want12));   // trailing comma

    value emptynl = VAL("{\n}");
    RC_CHECK_TRUE(value_is_list(emptynl));
    RC_CHECK(emptynl.list.num, ==, 0u);
}

RC_TEST_STEP(expression, list_errors, fix)
{
    RC_CHECK_TRUE(RESULT("{1").error    == expr_error_expected_close_brace);
    RC_CHECK_TRUE(RESULT("{1 2}").error == expr_error_expected_close_brace);   // missing comma
    RC_CHECK_TRUE(RESULT("{,}").error   == expr_error_expected_expression);
}

RC_TEST_STEP(expression, list_elementwise, fix)
{
    // A unary op or a function maps over every element, building a new list.
    value neg[] = {value_make_numeric(-1), value_make_numeric(-2), value_make_numeric(-3)};
    RC_CHECK_TRUE(value_is_equal(VAL("-{1,2,3}"), value_make_list((rc_view_value) RC_VIEW(neg))));

    value abs3[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3)};
    RC_CHECK_TRUE(value_is_equal(VAL("abs({-1,-2,-3})"), value_make_list((rc_view_value) RC_VIEW(abs3))));

    value ints[] = {value_make_numeric(2), value_make_numeric(-3)};
    RC_CHECK_TRUE(value_is_equal(VAL("int({2.7,-2.7})"), value_make_list((rc_view_value) RC_VIEW(ints))));

    // Nested lists map recursively: -{1,{2,3}} == {-1,{-2,-3}}.
    value inner[] = {value_make_numeric(-2), value_make_numeric(-3)};
    value nested[] = {value_make_numeric(-1), value_make_list((rc_view_value) RC_VIEW(inner))};
    RC_CHECK_TRUE(value_is_equal(VAL("-{1,{2,3}}"), value_make_list((rc_view_value) RC_VIEW(nested))));

    // An empty list maps to an empty list.
    value empty = VAL("-{}");
    RC_CHECK_TRUE(value_is_list(empty) && empty.list.num == 0);

    // An element the op can't take becomes an error in that slot; the rest are fine.
    value mixed = VAL("abs({-5, \"x\"})");
    RC_CHECK_TRUE(value_is_list(mixed) && mixed.list.num == 2);
    RC_CHECK_TRUE(value_is_equal(rc_view_value_get(mixed.list, 0), value_make_numeric(5)));
    RC_CHECK_TRUE(value_is_error(rc_view_value_get(mixed.list, 1)));
}

RC_TEST_STEP(expression, range_elementwise, fix)
{
    // A unary op or function over a range enumerates it and maps -> a list. Note the
    // parens: unary binds tighter than '..', so -(0..3) negates the range.
    value neg[] = {value_make_numeric(0), value_make_numeric(-1), value_make_numeric(-2), value_make_numeric(-3)};
    RC_CHECK_TRUE(value_is_equal(VAL("-(0..3)"), value_make_list((rc_view_value) RC_VIEW(neg))));

    value a[] = {value_make_numeric(2), value_make_numeric(1), value_make_numeric(0), value_make_numeric(1), value_make_numeric(2)};
    RC_CHECK_TRUE(value_is_equal(VAL("abs(-2..2)"), value_make_list((rc_view_value) RC_VIEW(a))));

    // Descending and stepped ranges enumerate correctly.
    value d[] = {value_make_numeric(-4), value_make_numeric(-3), value_make_numeric(-2), value_make_numeric(-1)};
    RC_CHECK_TRUE(value_is_equal(VAL("-(4..1)"), value_make_list((rc_view_value) RC_VIEW(d))));

    value s[] = {value_make_numeric(0), value_make_numeric(-2), value_make_numeric(-4), value_make_numeric(-6)};
    RC_CHECK_TRUE(value_is_equal(VAL("-(0..2..6)"), value_make_list((rc_view_value) RC_VIEW(s))));

    // An unbounded range can't be enumerated.
    RC_CHECK_TRUE(value_is_error(VAL("-(0..)")));
}

RC_TEST_STEP(expression, list_shape, fix)
{
    value s32[] = {value_make_numeric(3), value_make_numeric(2)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape({{1,2},{3,4},{5,6}})"), value_make_list((rc_view_value) RC_VIEW(s32))));

    value s3[] = {value_make_numeric(3)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape({1,2,3})"), value_make_list((rc_view_value) RC_VIEW(s3))));

    value sc = VAL("shape(5)");   // a scalar is rank 0
    RC_CHECK_TRUE(value_is_list(sc) && sc.list.num == 0);

    value sr[] = {value_make_numeric(2)};   // ragged: shape is the uniform prefix
    RC_CHECK_TRUE(value_is_equal(VAL("shape({1,{2,3}})"), value_make_list((rc_view_value) RC_VIEW(sr))));
}

RC_TEST_STEP(expression, list_broadcast, fix)
{
    value r1[] = {value_make_numeric(11), value_make_numeric(22), value_make_numeric(33)};
    RC_CHECK_TRUE(value_is_equal(VAL("{1,2,3}+{10,20,30}"), value_make_list((rc_view_value) RC_VIEW(r1))));

    value r2[] = {value_make_numeric(11), value_make_numeric(12), value_make_numeric(13)};
    RC_CHECK_TRUE(value_is_equal(VAL("{1,2,3}+10"), value_make_list((rc_view_value) RC_VIEW(r2))));
    RC_CHECK_TRUE(value_is_equal(VAL("10+{1,2,3}"), value_make_list((rc_view_value) RC_VIEW(r2))));

    RC_CHECK_TRUE(value_is_equal(VAL("5+3"), value_make_numeric(8)));   // two scalars stay scalar

    value r3[] = {value_make_numeric(10), value_make_numeric(40)};
    RC_CHECK_TRUE(value_is_equal(VAL("{1,2}*{10,20}"), value_make_list((rc_view_value) RC_VIEW(r3))));

    value r4[] = {value_make_numeric(0), value_make_numeric(1), value_make_numeric(0)};   // comparisons map (1/0)
    RC_CHECK_TRUE(value_is_equal(VAL("{1,2,3}=2"), value_make_list((rc_view_value) RC_VIEW(r4))));

    // nested: a scalar broadcasts into every leaf
    value n0[] = {value_make_numeric(11), value_make_numeric(12)};
    value n1[] = {value_make_numeric(13), value_make_numeric(14)};
    value nn[] = {value_make_list((rc_view_value) RC_VIEW(n0)), value_make_list((rc_view_value) RC_VIEW(n1))};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2},{3,4}}+10"), value_make_list((rc_view_value) RC_VIEW(nn))));

    // the 1-axis broadcast example: {{1},{2}}+{3,4} == {{4,5},{5,6}}
    value b0[] = {value_make_numeric(4), value_make_numeric(5)};
    value b1[] = {value_make_numeric(5), value_make_numeric(6)};
    value bb[] = {value_make_list((rc_view_value) RC_VIEW(b0)), value_make_list((rc_view_value) RC_VIEW(b1))};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1},{2}}+{3,4}"), value_make_list((rc_view_value) RC_VIEW(bb))));

    RC_CHECK_TRUE(value_is_error(VAL("{1,2,3}+{10,20,30,40}")));   // incompatible lengths

    // a ragged operand broadcasts structurally: {1,{2,3}}+1 == {2,{3,4}}
    value rag_inner[] = {value_make_numeric(3), value_make_numeric(4)};
    value rag[] = {value_make_numeric(2), value_make_list((rc_view_value) RC_VIEW(rag_inner))};
    RC_CHECK_TRUE(value_is_equal(VAL("{1,{2,3}}+1"), value_make_list((rc_view_value) RC_VIEW(rag))));

    // a range coerces to its rank-1 list under a binary op (parens: + binds tighter than ..)
    value cr1[] = {value_make_numeric(11), value_make_numeric(12), value_make_numeric(13)};
    RC_CHECK_TRUE(value_is_equal(VAL("(1..3)+10"), value_make_list((rc_view_value) RC_VIEW(cr1))));
    RC_CHECK_TRUE(value_is_equal(VAL("10+(1..3)"), value_make_list((rc_view_value) RC_VIEW(cr1))));

    value cr2[] = {value_make_numeric(11), value_make_numeric(22), value_make_numeric(33)};
    RC_CHECK_TRUE(value_is_equal(VAL("(1..3)+{10,20,30}"), value_make_list((rc_view_value) RC_VIEW(cr2))));

    value cr3[] = {value_make_numeric(11), value_make_numeric(13), value_make_numeric(15)};   // range + range
    RC_CHECK_TRUE(value_is_equal(VAL("(1..3)+(10..12)"), value_make_list((rc_view_value) RC_VIEW(cr3))));

    RC_CHECK_TRUE(value_is_error(VAL("(1..)+10")));   // an unbounded range cannot be enumerated

    // an element error stays element-local
    value mixed = VAL("{1,bar}+{10,20}");
    RC_CHECK_TRUE(value_is_list(mixed) && mixed.list.num == 2);
    RC_CHECK_TRUE(value_is_equal(rc_view_value_get(mixed.list, 0), value_make_numeric(11)));
    RC_CHECK_TRUE(value_is_error(rc_view_value_get(mixed.list, 1)));
}

RC_TEST_STEP(expression, ranges_two_value, fix)
{
    RC_CHECK_TRUE(range_is(VAL("0..9"),   0, 9, 0));     // step 0 = direction inferred
    RC_CHECK_TRUE(range_is(VAL("4..1"),   4, 1, 0));     // descending
    RC_CHECK_TRUE(range_is(VAL("5..5"),   5, 5, 0));     // single element
    RC_CHECK_TRUE(range_is(VAL("0..<10"), 0, 9, 0));     // exclusive end
}

RC_TEST_STEP(expression, ranges_stepped, fix)
{
    RC_CHECK_TRUE(range_is(VAL("1..3..7"),     1,  7,  2));
    RC_CHECK_TRUE(range_is(VAL("4..6..<10"),   4,  8,  2));   // exclusive, canonical end
    RC_CHECK_TRUE(range_is(VAL("10..8..2"),    10, 2, -2));   // descending step
    RC_CHECK_TRUE(range_is(VAL("1..3..5..9"),  1,  9,  2));   // chained, consistent
    RC_CHECK_TRUE(range_is(VAL("0..2..4..10"), 0,  10, 2));
    // same elements, however spelled, compare equal (canonical end)
    RC_CHECK_TRUE(value_is_equal(VAL("4..6..8"), VAL("4..6..<10")));
}

RC_TEST_STEP(expression, ranges_precedence, fix)
{
    RC_CHECK_TRUE(range_is(VAL("0..9+1"), 0, 10, 0));   // '+' binds tighter than '..'
    RC_CHECK_TRUE(value_is_list(VAL("(0..9)+1")));      // ( ) makes the range the operand; it coerces and broadcasts
}

RC_TEST_STEP(expression, ranges_unbounded, fix)
{
    value a = VAL("5..");
    RC_CHECK_TRUE(value_is_range(a) && a.range.has_start && !a.range.has_end
        && a.range.start == 5 && a.range.step == 0);

    value b = VAL("..5");
    RC_CHECK_TRUE(value_is_range(b) && !b.range.has_start && b.range.has_end && b.range.end == 5);

    value c = VAL("..");
    RC_CHECK_TRUE(value_is_range(c) && !c.range.has_start && !c.range.has_end);

    value d = VAL("0..2..");   // stepped, open end
    RC_CHECK_TRUE(value_is_range(d) && d.range.has_start && !d.range.has_end
        && d.range.start == 0 && d.range.step == 2);
}

RC_TEST_STEP(expression, ranges_errors, fix)
{
    RC_CHECK_TRUE(value_is_error(VAL("1..3..0")));      // not monotonic
    RC_CHECK_TRUE(value_is_error(VAL("0..3.5")));       // non-integer endpoint
    RC_CHECK_TRUE(value_is_error(VAL("5..<5")));        // empty
    RC_CHECK_TRUE(value_is_error(VAL("4..<1")));        // empty (descending exclusive)
    RC_CHECK_TRUE(value_is_error(VAL("0..<2..6")));     // '<' on the wrong separator
    RC_CHECK_TRUE(value_is_error(VAL("(1..2)..3")));    // a range can't be a start
    RC_CHECK_TRUE(value_is_error(VAL("1..3..6..9")));   // inconsistent step (2 then 3)
    RC_CHECK_TRUE(value_is_error(VAL("0..3..4..10")));  // inconsistent step (1 then 3)
    RC_CHECK_TRUE(value_is_error(VAL("..2..6")));       // start-open and stepped
    RC_CHECK_TRUE(value_is_error(VAL("bar..9")));       // unknown symbol propagates
}

RC_TEST_STEP(expression, subscript, fix)
{
    // L = {{1,2,3},{4,5,6}}, indexed inline.
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[1,2]"), value_make_numeric(6)));   // both int -> scalar

    value r456[] = {value_make_numeric(4), value_make_numeric(5), value_make_numeric(6)};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[1]"), value_make_list((rc_view_value) RC_VIEW(r456))));   // axis 1 whole

    value r23[] = {value_make_numeric(2), value_make_numeric(3)};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[0,1..2]"), value_make_list((rc_view_value) RC_VIEW(r23))));

    value r25[] = {value_make_numeric(2), value_make_numeric(5)};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[..,1]"), value_make_list((rc_view_value) RC_VIEW(r25))));

    // a list selector keeps the axis where the bare integer dropped it: {1} vs 1
    value c2[] = {value_make_numeric(2)};
    value c5[] = {value_make_numeric(5)};
    value cc[] = {value_make_list((rc_view_value) RC_VIEW(c2)), value_make_list((rc_view_value) RC_VIEW(c5))};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[..,{1}]"), value_make_list((rc_view_value) RC_VIEW(cc))));

    // orthogonal cross-product, not NumPy's zip
    value x0[] = {value_make_numeric(1), value_make_numeric(3)};
    value x1[] = {value_make_numeric(4), value_make_numeric(6)};
    value xx[] = {value_make_list((rc_view_value) RC_VIEW(x0)), value_make_list((rc_view_value) RC_VIEW(x1))};
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[{0,1},{0,2}]"), value_make_list((rc_view_value) RC_VIEW(xx))));

    // a range that clamps to empty -> an empty list, not an error
    value empty = VAL("{{1,2,3},{4,5,6}}[0,5..10]");
    RC_CHECK_TRUE(value_is_list(empty) && empty.list.num == 0);

    // chaining: L[1][2] == L[1,2]
    RC_CHECK_TRUE(value_is_equal(VAL("{{1,2,3},{4,5,6}}[1][2]"), value_make_numeric(6)));

    // errors on a dropped (integer) axis fail the whole subscript
    RC_CHECK_TRUE(value_is_error(VAL("{{1,2,3},{4,5,6}}[0,{0,5}]")));   // gather out of bounds
    RC_CHECK_TRUE(value_is_error(VAL("{{1,2,3},{4,5,6}}[5]")));         // integer out of bounds
    RC_CHECK_TRUE(value_is_error(VAL("{{1,2,3},{4,5,6}}[-1]")));        // negative (v1)
    RC_CHECK_TRUE(value_is_error(VAL("5[0]")));                         // a number is not subscriptable

    // an error under a kept axis stays element-local (like apply_binary)
    value rag = VAL("{{1,2},{3,4,5}}[..,2]");                           // row 0 has no index 2
    RC_CHECK_TRUE(value_is_list(rag) && rag.list.num == 2);
    RC_CHECK_TRUE(value_is_error(rc_view_value_get(rag.list, 0)));
    RC_CHECK_TRUE(value_is_equal(rc_view_value_get(rag.list, 1), value_make_numeric(5)));

    // indexing a range value via its enumeration
    RC_CHECK_TRUE(value_is_equal(VAL("(0..10)[2]"), value_make_numeric(2)));
    value r123[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3)};
    RC_CHECK_TRUE(value_is_equal(VAL("(0..10)[1..3]"), value_make_list((rc_view_value) RC_VIEW(r123))));
    RC_CHECK_TRUE(value_is_error(VAL("(0..)[0]")));   // an unbounded range cannot be enumerated

    // strings are rank-1 and always yield a string
    RC_CHECK_TRUE(value_is_equal(VAL("\"hello\"[0]"),      value_make_string(RC_STR("h"))));
    RC_CHECK_TRUE(value_is_equal(VAL("\"hello\"[1..3]"),   value_make_string(RC_STR("ell"))));
    RC_CHECK_TRUE(value_is_equal(VAL("\"hello\"[{0,4}]"),  value_make_string(RC_STR("ho"))));
    RC_CHECK_TRUE(value_is_equal(VAL("\"hello\"[..]"),     value_make_string(RC_STR("hello"))));
    RC_CHECK_TRUE(value_is_equal(VAL("\"hello\"[2..100]"), value_make_string(RC_STR("llo"))));   // clamps
    RC_CHECK_TRUE(value_is_error(VAL("\"hello\"[0,1]")));   // too many axes for a string

    // subscript binds tighter than unary and pow
    RC_CHECK_TRUE(value_is_equal(VAL("-{10,20,30}[0]"),  value_make_numeric(-10)));    // -(L[0])
    RC_CHECK_TRUE(value_is_equal(VAL("2^{10,20,30}[0]"), value_make_numeric(1024)));   // 2^(L[0])
    RC_CHECK_TRUE(value_is_equal(VAL("{10,20,30}[0]^2"), value_make_numeric(100)));    // (L[0])^2

    RC_CHECK_TRUE(RESULT("{1,2,3}[0").error != expr_error_none);   // a missing ']' is a committed error
    RC_CHECK_TRUE(RESULT("{1,2,3}[]").error != expr_error_none);   // an empty '[]' is illegal
    RC_CHECK_TRUE(RESULT("{1,2,3}[0,]").error != expr_error_none); // a trailing comma too
}

RC_TEST_STEP(expression, reductions, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("sum({1,2,3,4})"),         value_make_numeric(10)));
    RC_CHECK_TRUE(value_is_equal(VAL("product({1,2,3,4})"),     value_make_numeric(24)));
    RC_CHECK_TRUE(value_is_equal(VAL("min({3,1,2})"),           value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("max({3,1,2})"),           value_make_numeric(3)));
    RC_CHECK_TRUE(value_is_equal(VAL("sum({{1,2,3},{4,5,6}})"), value_make_numeric(21)));   // full reduction

    // collapse one axis: column sums (axis 0) and row sums (axis 1)
    value cols[] = {value_make_numeric(5), value_make_numeric(7), value_make_numeric(9)};
    RC_CHECK_TRUE(value_is_equal(VAL("sum({{1,2,3},{4,5,6}}, 0)"), value_make_list((rc_view_value) RC_VIEW(cols))));
    value rows[] = {value_make_numeric(6), value_make_numeric(15)};
    RC_CHECK_TRUE(value_is_equal(VAL("sum({{1,2,3},{4,5,6}}, 1)"), value_make_list((rc_view_value) RC_VIEW(rows))));

    // matrix * vector, the original motivation: sum(M*v, 1) with v = {1,0,0} picks column 0
    value mv[] = {value_make_numeric(1), value_make_numeric(4)};
    RC_CHECK_TRUE(value_is_equal(VAL("sum({{1,2,3},{4,5,6}} * {1,0,0}, 1)"), value_make_list((rc_view_value) RC_VIEW(mv))));

    // min/max over an axis pick element-wise vs per-row
    value mn0[] = {value_make_numeric(2), value_make_numeric(1)};
    RC_CHECK_TRUE(value_is_equal(VAL("min({{5,1},{2,8}}, 0)"), value_make_list((rc_view_value) RC_VIEW(mn0))));
    value mn1[] = {value_make_numeric(1), value_make_numeric(2)};
    RC_CHECK_TRUE(value_is_equal(VAL("min({{5,1},{2,8}}, 1)"), value_make_list((rc_view_value) RC_VIEW(mn1))));

    RC_CHECK_TRUE(value_is_equal(VAL("sum({})"), value_make_numeric(0)));   // identity 0
    RC_CHECK_TRUE(value_is_error(VAL("min({})")));                          // min has no identity
    RC_CHECK_TRUE(value_is_error(VAL("sum(5, 0)")));                        // axis out of range
}

RC_TEST_STEP(expression, list_functions, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("len({1,2,3})"),            value_make_numeric(3)));
    RC_CHECK_TRUE(value_is_equal(VAL("len(\"hello\")"),         value_make_numeric(5)));
    RC_CHECK_TRUE(value_is_equal(VAL("len({{1,2},{3,4},{5,6}})"), value_make_numeric(3)));
    RC_CHECK_TRUE(value_is_equal(VAL("rank(5)"),                 value_make_numeric(0)));
    RC_CHECK_TRUE(value_is_equal(VAL("rank({1,2,3})"),          value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("rank({{1,2},{3,4}})"),    value_make_numeric(2)));
    RC_CHECK_TRUE(value_is_error(VAL("len(5)")));                            // a scalar has no length

    value fl[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3), value_make_numeric(4), value_make_numeric(5)};
    RC_CHECK_TRUE(value_is_equal(VAL("flatten({1,{2,{3,4}},5})"), value_make_list((rc_view_value) RC_VIEW(fl))));
    RC_CHECK_TRUE(value_is_equal(VAL("concat({1,2}, 3, {4,5})"),  value_make_list((rc_view_value) RC_VIEW(fl))));

    value rv[] = {value_make_numeric(3), value_make_numeric(2), value_make_numeric(1)};
    RC_CHECK_TRUE(value_is_equal(VAL("reverse({1,2,3})"), value_make_list((rc_view_value) RC_VIEW(rv))));
    RC_CHECK_TRUE(value_is_equal(VAL("reverse(\"abc\")"), value_make_string(RC_STR("cba"))));

    value so[] = {value_make_numeric(1), value_make_numeric(2), value_make_numeric(3)};
    RC_CHECK_TRUE(value_is_equal(VAL("sort({3,1,2})"), value_make_list((rc_view_value) RC_VIEW(so))));

    // sort a list of rows keyed on each row's first element
    value k0[] = {value_make_numeric(1), value_make_numeric(10)};
    value k1[] = {value_make_numeric(2), value_make_numeric(20)};
    value k2[] = {value_make_numeric(3), value_make_numeric(30)};
    value ks[] = {value_make_list((rc_view_value) RC_VIEW(k0)), value_make_list((rc_view_value) RC_VIEW(k1)), value_make_list((rc_view_value) RC_VIEW(k2))};
    RC_CHECK_TRUE(value_is_equal(VAL("sort({{3,30},{1,10},{2,20}}, 0)"), value_make_list((rc_view_value) RC_VIEW(ks))));
}

RC_TEST_STEP(expression, defined_lohi_strings, fix)
{
    scopes_set_symbol(&fix->scopes, 0, RC_STR("foo"), value_make_numeric(42.0), (source_pos){0, 0});
    RC_CHECK_TRUE(value_is_equal(VAL("defined(foo)"), value_make_numeric(1)));   // resolves
    RC_CHECK_TRUE(value_is_equal(VAL("defined(bar)"), value_make_numeric(0)));   // an unknown symbol

    // '<' low byte, '>' high byte (6502 style), super low precedence so they grab the tail
    RC_CHECK_TRUE(value_is_equal(VAL("<258"),     value_make_numeric(2)));       // low byte of 0x102
    RC_CHECK_TRUE(value_is_equal(VAL(">258"),     value_make_numeric(1)));       // high byte
    RC_CHECK_TRUE(value_is_equal(VAL("<$1234"),   value_make_numeric(0x34)));
    RC_CHECK_TRUE(value_is_equal(VAL(">$1234"),   value_make_numeric(0x12)));
    RC_CHECK_TRUE(value_is_equal(VAL("<$1234+1"), value_make_numeric(0x35)));    // swallows the +1: lo($1234+1)

    // '+' concatenates strings
    RC_CHECK_TRUE(value_is_equal(VAL("\"foo\"+\"bar\""),      value_make_string(RC_STR("foobar"))));
    RC_CHECK_TRUE(value_is_equal(VAL("len(\"foo\"+\"bar\")"), value_make_numeric(6)));
}

RC_TEST_STEP(expression, math_functions, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("sin(0)"), value_make_numeric(0)));
    RC_CHECK_TRUE(value_is_equal(VAL("cos(0)"), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("exp(0)"), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("ln(1)"),  value_make_numeric(0)));

    RC_CHECK(VAL("log(1000)").numeric, ~=, 3.0);                  // base 10
    RC_CHECK(VAL("atan(1)").numeric,   ~=, 0.7853981633974483);   // pi/4
    RC_CHECK(VAL("asin(1)").numeric,   ~=, 1.5707963267948966);   // pi/2

    RC_CHECK_TRUE(value_is_error(VAL("asin(2)")));   // domain error (NaN)
    RC_CHECK_TRUE(value_is_error(VAL("ln(-1)")));    // domain error

    // element-wise over a list, like the other unary ops
    value c[] = {value_make_numeric(1)};
    RC_CHECK_TRUE(value_is_equal(VAL("cos({0})"), value_make_list((rc_view_value) RC_VIEW(c))));
}

RC_TEST_STEP(expression, strings_and_zip, fix)
{
    // escaped string literals: a doubled "" collapses to one quote
    RC_CHECK_TRUE(value_is_equal(VAL("\"a\"\"b\""), value_make_string(RC_STR("a\"b"))));   // "a""b" -> a"b
    RC_CHECK_TRUE(value_is_equal(VAL("\"\"\"\""),   value_make_string(RC_STR("\""))));     // """" -> "

    // string comparisons (equality and lexicographic ordering)
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" = \"abc\""), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" = \"abd\""), value_make_numeric(0)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" != \"abd\""), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" < \"abd\""), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abd\" > \"abc\""), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"ab\" < \"abc\""),  value_make_numeric(1)));   // prefix is less
    RC_CHECK_TRUE(value_is_error(VAL("\"a\" = 1")));                                  // mixed types

    // comparisons broadcast over a list of strings
    value cmp[] = {value_make_numeric(1), value_make_numeric(0)};
    RC_CHECK_TRUE(value_is_equal(VAL("{\"a\",\"b\"} = \"a\""), value_make_list((rc_view_value) RC_VIEW(cmp))));

    // zip: equal-length lists into tuples (transpose of the stacked args)
    value z0[] = {value_make_numeric(1), value_make_numeric(4)};
    value z1[] = {value_make_numeric(2), value_make_numeric(5)};
    value z2[] = {value_make_numeric(3), value_make_numeric(6)};
    value zz[] = {value_make_list((rc_view_value) RC_VIEW(z0)), value_make_list((rc_view_value) RC_VIEW(z1)), value_make_list((rc_view_value) RC_VIEW(z2))};
    RC_CHECK_TRUE(value_is_equal(VAL("zip({1,2,3},{4,5,6})"), value_make_list((rc_view_value) RC_VIEW(zz))));

    value t0[] = {value_make_numeric(1), value_make_numeric(3), value_make_numeric(5)};
    value t1[] = {value_make_numeric(2), value_make_numeric(4), value_make_numeric(6)};
    value tt[] = {value_make_list((rc_view_value) RC_VIEW(t0)), value_make_list((rc_view_value) RC_VIEW(t1))};
    RC_CHECK_TRUE(value_is_equal(VAL("zip({1,2},{3,4},{5,6})"), value_make_list((rc_view_value) RC_VIEW(tt))));

    RC_CHECK_TRUE(value_is_error(VAL("zip({1,2},{3,4,5})")));   // lengths must match
}

#undef RESULT
#undef VAL

#endif // BARON_TESTS
