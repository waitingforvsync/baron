#include "expression.h"

#include "lexer.h"
#include "richc/array/u32.h"   // rc_array_u32, for the indices a subscript selector picks
#include "richc/macros.h"
#include <math.h>


// ---- precedence ladder (higher binds tighter) ----
// The range operator '..' is the loosest, so its endpoints are whole expressions;
// comparisons and the logical ops sit below arithmetic; shifts share the multiply
// level; unary +/- sit below pow so -2^2 is -(2^2).
typedef enum prec {
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
    (void)arena;
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

// Comparisons yield 1 for true, 0 for false.
static value op_eq(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric == b.numeric ? 1.0 : 0.0);
}

static value op_ne(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric != b.numeric ? 1.0 : 0.0);
}

static value op_lt(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric < b.numeric ? 1.0 : 0.0);
}

static value op_gt(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric > b.numeric ? 1.0 : 0.0);
}

static value op_le(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric <= b.numeric ? 1.0 : 0.0);
}

static value op_ge(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric(a.numeric >= b.numeric ? 1.0 : 0.0);
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
static value fn_shape(value v, rc_arena *arena)
{
    uint32_t dims[MAX_RANK];
    uint32_t rank = shape_dims(v, dims, MAX_RANK);
    rc_array_value out = {0};
    for (uint32_t i = 0; i < rank; i++) {
        rc_array_value_push(&out, value_make_numeric((double)dims[i]), arena);
    }
    return value_make_list(out.view);
}


// ---- the two context tables ----
// EVEN: lexed where an operand is expected (the start, after a binary op, after an
// open paren). Numbers/strings/identifiers come from the lexer itself, so the table
// only carries the leading operators, the functions, and the open paren.
static const token even_entries[] = {
    {RC_STR("("),     {.type = lexeme_type_open_paren}},
    {RC_STR("{"),     {.type = lexeme_type_open_brace}},     // begins a list literal
    {RC_STR("}"),     {.type = lexeme_type_close_brace}},    // ends one (empty, or after a comma)
    {RC_STR(".."),    {.type = lexeme_type_range, .range = {false}}},   // start-unbounded range
    {RC_STR("..<"),   {.type = lexeme_type_range, .range = {true}}},
    {RC_STR("+"),     {.type = lexeme_type_unary_op, .unary_op = {op_pos,   prec_neg}}},
    {RC_STR("-"),     {.type = lexeme_type_unary_op, .unary_op = {op_neg,   prec_neg}}},
    {RC_STR("abs"),   {.type = lexeme_type_function, .function = {fn_abs,   false}}},
    {RC_STR("lo"),    {.type = lexeme_type_function, .function = {fn_lo,    false}}},
    {RC_STR("hi"),    {.type = lexeme_type_function, .function = {fn_hi,    false}}},
    {RC_STR("sqrt"),  {.type = lexeme_type_function, .function = {fn_sqrt,  false}}},
    {RC_STR("not"),   {.type = lexeme_type_function, .function = {fn_not,   false}}},
    {RC_STR("int"),   {.type = lexeme_type_function, .function = {fn_int,   false}}},
    {RC_STR("floor"), {.type = lexeme_type_function, .function = {fn_int,   false}}},   // alias of int
    {RC_STR("round"), {.type = lexeme_type_function, .function = {fn_round, false}}},
    {RC_STR("ceil"),  {.type = lexeme_type_function, .function = {fn_ceil,  false}}},
    {RC_STR("shape"), {.type = lexeme_type_function, .function = {fn_shape, true }}},   // aggregate
};

// ODD: lexed where a binary operator is expected (after an operand). The close paren
// lives here, so a parenthesised group is closed from operator position.
static const token odd_entries[] = {
    {RC_STR(")"),   {.type = lexeme_type_close_paren}},
    {RC_STR("}"),   {.type = lexeme_type_close_brace}},   // ends a list (after an element)
    {RC_STR("["),   {.type = lexeme_type_open_bracket}},  // postfix subscript
    {RC_STR("]"),   {.type = lexeme_type_close_bracket}}, // ends a subscript (after a selector)
    {RC_STR(".."),  {.type = lexeme_type_range, .range = {false}}},   // range operator (handled specially)
    {RC_STR("..<"), {.type = lexeme_type_range, .range = {true}}},
    {RC_STR("^"),   {.type = lexeme_type_binary_op, .binary_op = {op_pow,  prec_pow, assoc_right}}},
    {RC_STR("*"),   {.type = lexeme_type_binary_op, .binary_op = {op_mul,  prec_mul, assoc_left}}},
    {RC_STR("/"),   {.type = lexeme_type_binary_op, .binary_op = {op_div,  prec_mul, assoc_left}}},
    {RC_STR("div"), {.type = lexeme_type_binary_op, .binary_op = {op_idiv, prec_mul, assoc_left}}},
    {RC_STR("mod"), {.type = lexeme_type_binary_op, .binary_op = {op_mod,  prec_mul, assoc_left}}},
    {RC_STR("<<"),  {.type = lexeme_type_binary_op, .binary_op = {op_shl,  prec_mul, assoc_left}}},
    {RC_STR(">>"),  {.type = lexeme_type_binary_op, .binary_op = {op_shr,  prec_mul, assoc_left}}},
    {RC_STR("+"),   {.type = lexeme_type_binary_op, .binary_op = {op_add,  prec_add, assoc_left}}},
    {RC_STR("-"),   {.type = lexeme_type_binary_op, .binary_op = {op_sub,  prec_add, assoc_left}}},
    {RC_STR("="),   {.type = lexeme_type_binary_op, .binary_op = {op_eq,   prec_cmp, assoc_left}}},
    {RC_STR("=="),  {.type = lexeme_type_binary_op, .binary_op = {op_eq,   prec_cmp, assoc_left}}},   // alias of =
    {RC_STR("!="),  {.type = lexeme_type_binary_op, .binary_op = {op_ne,   prec_cmp, assoc_left}}},
    {RC_STR("<>"),  {.type = lexeme_type_binary_op, .binary_op = {op_ne,   prec_cmp, assoc_left}}},   // alias of !=
    {RC_STR("<="),  {.type = lexeme_type_binary_op, .binary_op = {op_le,   prec_cmp, assoc_left}}},
    {RC_STR(">="),  {.type = lexeme_type_binary_op, .binary_op = {op_ge,   prec_cmp, assoc_left}}},
    {RC_STR("<"),   {.type = lexeme_type_binary_op, .binary_op = {op_lt,   prec_cmp, assoc_left}}},
    {RC_STR(">"),   {.type = lexeme_type_binary_op, .binary_op = {op_gt,   prec_cmp, assoc_left}}},
    {RC_STR("and"), {.type = lexeme_type_binary_op, .binary_op = {op_and,  prec_and, assoc_left}}},
    {RC_STR("or"),  {.type = lexeme_type_binary_op, .binary_op = {op_or,   prec_or,  assoc_left}}},
    {RC_STR("eor"), {.type = lexeme_type_binary_op, .binary_op = {op_eor,  prec_or,  assoc_left}}},
};

static const token_table even_tokens = RC_VIEW(even_entries);
static const token_table odd_tokens  = RC_VIEW(odd_entries);


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
    int64_t step = r.step != 0 ? r.step : (r.end >= r.start ? 1 : -1);   // step 0 = inferred +-1
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

// A scalar applies directly; a list maps element-wise, recursing so nested lists map
// too; an error short-circuits. The new list is built in the (scratch) arena and
// wrapped - value_make_list does not copy.
static value apply_elementwise(value (*scalar)(value, rc_arena *), value v, rc_arena *arena)
{
    if (value_is_error(v)) {
        return v;
    }
    if (value_is_list(v)) {
        rc_array_value out = {0};
        for (uint32_t i = 0; i < v.list.num; i++) {
            rc_array_value_push(&out, apply_elementwise(scalar, rc_view_value_get(v.list, i), arena), arena);
        }
        return value_make_list(out.view);
    }
    if (value_is_range(v)) {
        // A range maps as its enumerated list would; range_to_list carries the unbounded
        // case through as an error, which the recursive call passes straight back.
        return apply_elementwise(scalar, range_to_list(v.range, arena), arena);
    }
    return scalar(v, arena);
}

static value apply_unary(lexeme_unary_op op, value v, rc_arena *arena)
{
    return apply_elementwise(op.apply, v, arena);
}

static value apply_function(lexeme_function fn, value v, rc_arena *arena)
{
    // An aggregate function (e.g. shape) takes the whole value; the rest map element-wise.
    if (fn.aggregate) {
        return value_is_error(v) ? v : fn.apply(v, arena);
    }
    return apply_elementwise(fn.apply, v, arena);
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
    int64_t step = r.step != 0      ? r.step
                 : r.has_start && r.has_end ? (r.end >= r.start ? 1 : -1)
                 : 1;   // unbounded: enumerate the axis ascending
    int64_t start = r.has_start ? r.start : (step > 0 ? 0 : (int64_t)len - 1);
    int64_t end   = r.has_end   ? r.end   : (step > 0 ? (int64_t)len - 1 : 0);
    if (step > 0 && end > (int64_t)len - 1) end = (int64_t)len - 1;   // clamp the far end so the
    if (step < 0 && end < 0)                end = 0;                  // loop cannot run off the axis
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
static value subscript_string(rc_str s, rc_view_value sels, uint32_t k, rc_arena *arena)
{
    if (sels.num - k != 1) {
        return value_make_error(value_error_subscript_range);   // too many axes for a string
    }
    value sel = rc_view_value_get(sels, k);

    if (value_is_numeric(sel)) {
        uint32_t i = selector_index(sel, s.len);
        if (i == RC_INDEX_NONE) {
            return value_make_error(value_error_subscript_range);
        }
        return value_make_string(rc_str_substr(s, i, 1));
    }

    rc_mstr m = rc_mstr_make(s.len, arena);   // gather the picked characters
    if (value_is_range(sel)) {
        rc_array_u32 idx = range_indices(sel.range, s.len, arena);
        for (uint32_t j = 0; j < idx.num; j++) {
            rc_mstr_append_char(&m, s.data[RC_AT(idx, j)], arena);
        }
    } else if (value_is_list(sel)) {
        for (uint32_t j = 0; j < sel.list.num; j++) {
            uint32_t i = selector_index(rc_view_value_get(sel.list, j), s.len);
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

// Index a value by the remaining selectors sels[k..]. Recurses per selected element, so
// each selector applies to one axis and the selectors cross-product. A spent selector list
// returns the value whole (trailing axes untouched); a non-subscriptable value errors.
static value subscript(value v, rc_view_value sels, uint32_t k, rc_arena *arena)
{
    if (value_is_error(v)) {
        return v;
    }
    if (k == sels.num) {
        return v;   // no more selectors: this axis and anything within it taken whole
    }
    if (value_is_string(v)) {
        return subscript_string(v.string, sels, k, arena);
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

    value sel = rc_view_value_get(sels, k);
    uint32_t len = v.list.num;

    if (value_is_numeric(sel)) {   // an integer drops this axis
        uint32_t i = selector_index(sel, len);
        if (i == RC_INDEX_NONE) {
            return value_make_error(value_error_subscript_range);
        }
        return subscript(rc_view_value_get(v.list, i), sels, k + 1, arena);
    }

    // a range or list keeps this axis: recurse on each selected element into a result list
    rc_array_value out = {0};
    if (value_is_range(sel)) {
        rc_array_u32 idx = range_indices(sel.range, len, arena);
        for (uint32_t j = 0; j < idx.num; j++) {
            value e = subscript(rc_view_value_get(v.list, RC_AT(idx, j)), sels, k + 1, arena);
            rc_array_value_push(&out, e, arena);
        }
    } else if (value_is_list(sel)) {
        for (uint32_t j = 0; j < sel.list.num; j++) {
            uint32_t i = selector_index(rc_view_value_get(sel.list, j), len);
            if (i == RC_INDEX_NONE) {
                return value_make_error(value_error_subscript_range);
            }
            value e = subscript(rc_view_value_get(v.list, i), sels, k + 1, arena);
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
        case lexeme_type_escaped_string_literal:
            // TODO: do this properly for the escaped case - the ref still holds its
            // doubled quotes verbatim, so we must build a fresh copy (in the scratch
            // arena) with the escapes collapsed rather than wrapping the raw source.
            return ok(value_make_string(lex.string_literal.ref), lr.next);

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
            // The operand is parsed at the operator's own precedence (prefix climb).
            expr_result arg = parse_precedence(p, lr.next, lex.unary_op.precedence);
            if (arg.error != expr_error_none) {
                return arg;   // nothing to apply the operator to: soft fail bubbles up
            }
            return ok(apply_unary(lex.unary_op, arg.value, p->arena), arg.next);
        }

        case lexeme_type_function: {
            lexer_result lp = lexer_next(p->text, lr.next, even_tokens);
            if (lp.token.type != lexeme_type_open_paren) {
                return fail(expr_error_expected_open_paren, lr.next);
            }
            expr_result arg = parse_precedence(p, lp.next, 0);
            if (arg.error != expr_error_none) {
                return arg;
            }
            expr_result closed = expect_close_paren(p, arg.value, arg.next);
            if (closed.error != expr_error_none) {
                return closed;
            }
            return ok(apply_function(lex.function, closed.value, p->arena), closed.next);
        }

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
    rc_array_value sels = {0};

    while (true) {
        expr_result s = parse_precedence(p, cursor, 0);
        if (s.error == expr_error_expected_expression) {
            return fail(expr_error_expected_expression, cursor);   // empty "[]" or a trailing comma
        }
        if (s.error != expr_error_none) {
            return s;
        }
        rc_array_value_push(&sels, s.value, p->arena);
        cursor = s.next;

        lexer_result lr = lexer_next(p->text, cursor, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            cursor = lr.next;
            continue;
        }
        if (lr.token.type == lexeme_type_close_bracket) {
            return ok(subscript(target, sels.view, 0, p->arena), lr.next);
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

RC_TEST_STEP(expression, symbols, fix)
{
    scopes_set_symbol(&fix->scopes, 0, RC_STR("foo"), value_make_numeric(42.0));

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
    RC_CHECK_TRUE(RESULT("(1+2").error  == expr_error_expected_close_paren);
    RC_CHECK_TRUE(RESULT("ABS x").error == expr_error_expected_open_paren);
    RC_CHECK_TRUE(RESULT("ABS(3").error == expr_error_expected_close_paren);
    RC_CHECK_TRUE(RESULT(")").error     == expr_error_expected_expression);
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

#undef RESULT
#undef VAL

#endif // BARON_TESTS
