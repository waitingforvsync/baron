#include "expression.h"

#include "scopes.h"   // scopes_get_symbol / _set_symbol / _get_or_make_child (a FUNCTION body binds locals)
#include "functions.h"   // the FUNCTION registry: functions_match, function_signature
#include "source_files.h"   // source_files_text: a body's source may differ from the call's
#include "lexer.h"
#include "richc/random.h"   // the RND stream
#include "richc/array/u32.h"   // rc_array_u32, for the indices a subscript selector picks
#include "richc/mstr.h"   // rc_mstr, to build a call's child-scope key
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

#define NEEDS_NUM(cond) do { if (!(cond)) return value_make_error(error_type_type_mismatch); } while (0)

// Coerce a number to 32 bits, truncating toward zero. The int64 hop makes the
// truncation well-defined before the 32-bit wrap. Bitwise ops read the value as
// unsigned (so >> is logical and NOT yields a positive pattern); div/mod read it
// signed.
static int32_t  as_i32(value v) { return (int32_t)(int64_t)v.numeric; }
static uint32_t as_u32(value v) { return (uint32_t)(int64_t)v.numeric; }

// Adjust a za_auto address by a numeric delta. The offset must stay an integral, non-negative byte
// index into the variable; whether it stays within the declared WIDTH is checked later, at the
// operand bounds check, where the width is known.
static value zp_offset_add(value zp, double delta)
{
    if (floor(delta) != delta) {
        return value_make_error(error_type_domain);
    }
    int64_t off = (int64_t) zp.za_auto.offset + (int64_t) delta;
    if (off < 0 || off > INT32_MAX) {
        return value_make_error(error_type_domain);
    }
    zp.za_auto.offset = (int32_t) off;
    return zp;
}

static value op_add(value a, value b, rc_arena *arena)
{
    if (value_is_string(a) && value_is_string(b)) {
        rc_mstr m = rc_mstr_make(a.string.len + b.string.len, arena);
        rc_mstr_append(&m, a.string, arena);
        rc_mstr_append(&m, b.string, arena);
        return value_make_string(m.view);   // '+' concatenates two strings
    }
    // A za_auto address plus an integer is the same address further in: ptr+1 is the pointer's high
    // byte, whichever byte the allocator eventually picks. This and subtraction below are the ONLY
    // arithmetic a za_auto value supports - every other operator's numeric check refuses it.
    if (value_is_za_auto(a) && value_is_number(b)) {
        return zp_offset_add(a, b.numeric);
    }
    if (value_is_number(a) && value_is_za_auto(b)) {
        return zp_offset_add(b, a.numeric);
    }
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    return value_make_numeric(a.numeric + b.numeric);
}

static value op_sub(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (value_is_za_auto(a) && value_is_number(b)) {
        return zp_offset_add(a, -b.numeric);
    }
    if (value_is_za_auto(a) && value_is_za_auto(b)) {
        // The distance between two offsets into the SAME variable is a plain number; two different
        // variables have no knowable distance before allocation.
        if (a.za_auto.scope == b.za_auto.scope && cursor_is_equal(a.za_auto.def, b.za_auto.def)) {
            return value_make_numeric((double) a.za_auto.offset - (double) b.za_auto.offset);
        }
        return value_make_error(error_type_domain);
    }
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    return value_make_numeric(a.numeric - b.numeric);
}

static value op_mul(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    return value_make_numeric(a.numeric * b.numeric);
}

static value op_div(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    if (b.numeric == 0.0) {
        return value_make_error(error_type_divide_by_zero);
    }
    return value_make_numeric(a.numeric / b.numeric);
}

static value op_pow(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    double r = pow(a.numeric, b.numeric);
    if (isnan(r)) {
        return value_make_error(error_type_domain);   // e.g. a negative base, fractional exponent
    }
    return value_make_numeric(r);
}

// Integer division and modulo, signed and truncating toward zero (C semantics).
static value op_idiv(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    int32_t ib = as_i32(b);
    if (ib == 0) {
        return value_make_error(error_type_divide_by_zero);
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
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    int32_t ib = as_i32(b);
    if (ib == 0) {
        return value_make_error(error_type_divide_by_zero);
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
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    uint32_t n = as_u32(b);
    return value_make_numeric(n >= 32 ? 0.0 : (double)(as_u32(a) << n));
}

static value op_shr(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    uint32_t n = as_u32(b);
    return value_make_numeric(n >= 32 ? 0.0 : (double)(as_u32(a) >> n));
}

// AND, OR and EOR are overloaded on their operand types: two booleans get the logical
// operation (yielding a boolean), two numbers the bitwise one on the 32-bit pattern.
// A mixed pair is refused rather than coerced - there is no honest reading of
// TRUE AND 4, so we make the caller say which they meant.
static value op_and(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (value_is_boolean(a) && value_is_boolean(b)) {
        return value_make_bool(a.numeric != 0.0 && b.numeric != 0.0);
    }
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric((double)(as_u32(a) & as_u32(b)));
}

static value op_or(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (value_is_boolean(a) && value_is_boolean(b)) {
        return value_make_bool(a.numeric != 0.0 || b.numeric != 0.0);
    }
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric((double)(as_u32(a) | as_u32(b)));
}

static value op_eor(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (value_is_boolean(a) && value_is_boolean(b)) {
        return value_make_bool((a.numeric != 0.0) != (b.numeric != 0.0));
    }
    NEEDS_NUM(value_is_numeric(a) && value_is_numeric(b));
    return value_make_numeric((double)(as_u32(a) ^ as_u32(b)));
}

// Order two operands of the same kind - both numbers, or both strings (lexicographic) -
// as order -1 / 0 / +1; ok false on a type mismatch, which makes the comparison an error.
typedef struct compare_result {
    int  order;
    bool ok;
} compare_result;

static compare_result compare_values(value a, value b)
{
    if (value_is_number(a) && value_is_number(b)) {
        return (compare_result) {.order = (a.numeric > b.numeric) - (a.numeric < b.numeric), .ok = true};
    }
    if (value_is_string(a) && value_is_string(b)) {
        int c = rc_str_compare(a.string, b.string);
        return (compare_result) {.order = (c > 0) - (c < 0), .ok = true};
    }
    return (compare_result) {0};
}

// The comparisons each yield a boolean, reading the ordering from compare_values
// (so they all work on numbers or on strings).
static value op_eq(value a, value b, rc_arena *arena)
{
    (void)arena;
    compare_result r = compare_values(a, b);
    return r.ok ? value_make_bool(r.order == 0) : value_make_error(error_type_type_mismatch);
}

static value op_ne(value a, value b, rc_arena *arena)
{
    (void)arena;
    compare_result r = compare_values(a, b);
    return r.ok ? value_make_bool(r.order != 0) : value_make_error(error_type_type_mismatch);
}

static value op_lt(value a, value b, rc_arena *arena)
{
    (void)arena;
    compare_result r = compare_values(a, b);
    return r.ok ? value_make_bool(r.order < 0) : value_make_error(error_type_type_mismatch);
}

static value op_gt(value a, value b, rc_arena *arena)
{
    (void)arena;
    compare_result r = compare_values(a, b);
    return r.ok ? value_make_bool(r.order > 0) : value_make_error(error_type_type_mismatch);
}

static value op_le(value a, value b, rc_arena *arena)
{
    (void)arena;
    compare_result r = compare_values(a, b);
    return r.ok ? value_make_bool(r.order <= 0) : value_make_error(error_type_type_mismatch);
}

static value op_ge(value a, value b, rc_arena *arena)
{
    (void)arena;
    compare_result r = compare_values(a, b);
    return r.ok ? value_make_bool(r.order >= 0) : value_make_error(error_type_type_mismatch);
}

// The fold operators behind min()/max(); they are not in the tables, only used by reduce().
static value op_min(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    return value_make_numeric(a.numeric < b.numeric ? a.numeric : b.numeric);
}

static value op_max(value a, value b, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(a) && value_is_number(b));
    return value_make_numeric(a.numeric > b.numeric ? a.numeric : b.numeric);
}

static value op_neg(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric(-v.numeric);
}

static value op_pos(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return v;
}


// ---- RND: the one impure builtin ----
// A module-static stream, reseeded to a fixed point at the start of every pass (expression_reset_random,
// called by the assembler's pass driver). That is what keeps RND reproducible pass-to-pass, so an assembly
// that uses it can still reach a fixpoint - a draw's value depends only on its position in the pass's parse.
#define EXPR_RANDOM_SEED 0u
static rc_random expr_prng;

void expression_reset_random(void)
{
    expr_prng = rc_random_make(EXPR_RANDOM_SEED);
}

// RND(n) -> one integer in [0, n). A unary op, so apply_unary maps it element-wise over a list/range (each
// element supplying its own bound) and screens error values first - this only ever sees one scalar, and
// draws exactly once, so the number of draws tracks the number of outputs (structurally determined).
static value fn_rnd(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    int64_t bound = (int64_t)v.numeric;   // truncate toward zero: RND(2.9) == RND(2)
    if (bound <= 0) {
        return value_make_error(error_type_domain);   // no 0..n-1 range for n <= 0
    }
    return value_make_numeric((double)((uint64_t)rc_random_next(&expr_prng) % (uint64_t)bound));
}

static value fn_abs(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric(fabs(v.numeric));
}

// Low and high bytes of the 32-bit pattern.
static value fn_lo(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric((double)(as_u32(v) & 0xFFu));
}

static value fn_hi(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric((double)((as_u32(v) >> 8) & 0xFFu));
}

// A number as an uppercase hex string, in the narrowest of richc's fixed widths that holds it: a byte, a
// word, or the whole 32 bits. 10 -> "0A", &123 -> "0123", &123456 -> "00123456". No "&" - that is the
// caller's to write, and PRINT is the point of the thing.
static value fn_hex(value v, rc_arena *arena)
{
    // A ZA_AUTO address stands in with its offset until allocation, exactly as an instruction operand
    // does; the real byte arrives on the output pass, which is the pass PRINT speaks on.
    NEEDS_NUM(value_is_number(v) || value_is_za_auto(v));
    uint32_t u = value_is_za_auto(v) ? (uint32_t) v.za_auto.offset : as_u32(v);

    rc_mstr m = rc_mstr_make(8, arena);
    if (u <= 0xFFu) {
        rc_mstr_append_hex8(&m, (uint8_t) u, arena);
    }
    else if (u <= 0xFFFFu) {
        rc_mstr_append_hex16(&m, (uint16_t) u, arena);
    }
    else {
        rc_mstr_append_hex32(&m, u, arena);
    }
    return value_make_string(m.view);
}

// A string's character codes as a rank-1 list of numbers: codes("AB") is {65, 66}, codes("") the
// empty list. This is the bridge from text to arithmetic - subscript for one character's code
// (codes("A")[0]), broadcasting for whole-string remaps (codes(s) - codes(" ")), and a gather
// subscript for the full character-mapping idiom (table[codes(s)]).
static value fn_codes(value v, rc_arena *arena)
{
    if (!value_is_string(v)) {
        return value_make_error(error_type_type_mismatch);
    }

    rc_array_value out = rc_array_value_make(v.string.len, arena);
    for (uint32_t i = 0; i < v.string.len; i++) {
        rc_array_value_push(&out, value_make_numeric((double) (uint8_t) v.string.data[i]), arena);
    }
    return value_make_list(out.view);
}

// Overloaded like AND/OR/EOR: logical on a boolean, bitwise complement on a number.
// NOT(TRUE) really is FALSE now.
static value fn_not(value v, rc_arena *arena)
{
    (void)arena;
    if (value_is_boolean(v)) {
        return value_make_bool(v.numeric == 0.0);
    }
    NEEDS_NUM(value_is_numeric(v));
    return value_make_numeric((double)(~as_u32(v)));
}

static value fn_sqrt(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    double r = sqrt(v.numeric);
    if (isnan(r)) {
        return value_make_error(error_type_domain);   // negative argument
    }
    return value_make_numeric(r);
}

// Rounding to an integral value, the four flavours: down, toward zero, to nearest, up.
static value fn_int(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric(floor(v.numeric));
}

static value fn_trunc(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric(trunc(v.numeric));
}

static value fn_round(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric(round(v.numeric));   // halves go away from zero: round(2.5) = 3, round(-2.5) = -3
}

static value fn_ceil(value v, rc_arena *arena)
{
    (void)arena;
    NEEDS_NUM(value_is_number(v));
    return value_make_numeric(ceil(v.numeric));
}

// Apply a one-argument C math function, turning a NaN result (a domain error such as
// asin(2) or ln(-1)) into a domain error value. The trig/log builtins are thin wrappers.
static value math1(double (*f)(double), value v)
{
    NEEDS_NUM(value_is_number(v));
    double r = f(v.numeric);
    if (isnan(r)) {
        return value_make_error(error_type_domain);
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

// The element count of a bounded range: (end - start) / step + 1 (the constructor stores end as the
// canonical last element, so the division is exact). Saturates at UINT32_MAX, far beyond anything
// materialisable - shape is pure arithmetic and never builds the list.
static uint32_t range_count(value_range r)
{
    int64_t step = value_range_step(r);
    int64_t n    = (r.end - r.start) / step + 1;
    return n > (int64_t) UINT32_MAX ? UINT32_MAX : (uint32_t) n;
}

// The uniform-length-prefix shape of v written into dims[], returning its rank: descend
// while every node at a level is a list of the same length, stopping at the first axis
// that is not uniform (so a ragged list reports the axes that ARE uniform). A scalar is
// rank 0; a leading length-1 axis is kept, not squashed. A bounded range shapes as the
// rank-1 list it stands for ({count}); an unbounded one has no honest length to report,
// so it degrades to rank 0 here (fn_shape errors on it at top level).
//
// The whole thing rests on one recursive idea: a list's shape is its own length, followed
// by the shape that ALL of its elements agree on. So {{1,2},{3,4}} is 2-of-(things that
// are each {2}) -> {2,2}; but {{1,2},{3,4,5}} is 2-of-(a {2} and a {3}, which agree on
// nothing) -> just {2}. "What they all agree on" is the common leading prefix of the
// elements' shapes, and each element can only ever trim that prefix shorter.
typedef struct shape {
    uint32_t rank;
    uint32_t dims[MAX_RANK];
} shape;

static shape shape_of(value v, uint32_t max)
{
    if (max == 0) {
        return (shape) {0};   // out of room: report no further axes
    }
    if (value_is_range(v)) {
        if (!v.range.has_start || !v.range.has_end) {
            return (shape) {0};   // unbounded: no length to report
        }
        // A bounded range is a compact rank-1 list of numbers.
        return (shape) {.rank = 1, .dims[0] = range_count(v.range)};
    }
    if (!value_is_list(v)) {
        return (shape) {0};   // a scalar: empty shape, rank 0
    }
    if (v.list.num == 0) {
        return (shape) {.rank = 1};   // empty list: shape {0}, no elements to descend into
    }

    // Element 0 proposes the inner shape; every other element gets a say, but only to trim -
    // we keep however many leading axes still match. The moment inner hits 0 there is nothing
    // left to agree on, so we stop looking (the inner > 0 guard).
    shape first = shape_of(rc_view_value_get(v.list, 0), max - 1);
    uint32_t inner = first.rank;
    for (uint32_t i = 1; i < v.list.num && inner > 0; i++) {
        shape other = shape_of(rc_view_value_get(v.list, i), max - 1);
        uint32_t common = 0;
        while (common < inner && common < other.rank && first.dims[common] == other.dims[common]) {
            common++;
        }
        inner = common;   // the agreed prefix can only shrink, never grow
    }

    // Axis 0 is always just how many things we are holding, then the surviving inner axes.
    shape s = {.rank = 1 + inner, .dims[0] = v.list.num};
    for (uint32_t k = 0; k < inner; k++) {
        s.dims[1 + k] = first.dims[k];
    }
    return s;
}

// The rank of v: the length of its uniform-length-prefix shape (0 for a scalar).
static uint32_t rank_of(value v)
{
    return shape_of(v, MAX_RANK).rank;
}

// The shape() function: the axis lengths as a list of numbers (a scalar -> the empty list).
// A function, so it takes the whole argument list and checks it wants exactly one.
static value fn_shape(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;   // functions get raw args now, so propagate an error operand ourselves
    }
    if (value_is_range(v) && (!v.range.has_start || !v.range.has_end)) {
        return value_make_error(error_type_domain);   // an unbounded range has no length to report
    }

    shape s = shape_of(v, MAX_RANK);

    rc_array_value out = {0};
    for (uint32_t i = 0; i < s.rank; i++) {
        rc_array_value_push(&out, value_make_numeric((double)s.dims[i]), arena);
    }

    return value_make_list(out.view);
}


// ---- the parser ----
// The invariants for one parse, bundled so the recursive helpers stay readable.
typedef struct parser {
    rc_str          text;
    const expr_env *env;
    rc_arena       *arena;
    bool            live;   // false during a FUNCTION-body definition scan OR inside a dead body branch:
                            //   a user-FUNCTION call short-circuits (no body run) rather than executing. A
                            //   dead branch is still PARSED, to find where it ends, so its calls must not run.
} parser;

// Expand a bounded range into its rank-1 list of numeric values; an unbounded range has
// no end to count to, so it cannot be enumerated and yields a domain error instead.
// Declared in expression.h (FOR walks a range with it).
value range_to_list(value_range r, rc_arena *arena)
{
    if (!r.has_start || !r.has_end) {
        return value_make_error(error_type_domain);
    }

    int64_t step = value_range_step(r);
    rc_array_value out = {0};
    for (int64_t n = r.start; step > 0 ? n <= r.end : n >= r.end; n += step) {
        if (out.view.num >= VALUE_LIST_MAX_LENGTH) {
            return value_make_error(error_type_list_too_big);   // stop before an unbounded allocation
        }
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
        return value_make_error(error_type_shape_mismatch);
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
    if (!value_is_number(sel) || sel.numeric < 0.0 || sel.numeric >= (double)len) {
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
        return value_make_error(error_type_subscript_range);   // a string takes exactly one axis
    }

    value index = rc_view_value_get(indices, 0);

    if (value_is_number(index)) {
        uint32_t i = selector_index(index, s.len);

        if (i == RC_INDEX_NONE) {
            return value_make_error(error_type_subscript_range);
        }

        return value_make_string(rc_str_substr(s, i, 1));
    }

    rc_mstr m = rc_mstr_make(s.len, arena);   // gather the picked characters
    if (value_is_range(index)) {
        rc_array_u32 idx = range_indices(index.range, s.len, arena);
        for (uint32_t j = 0; j < idx.num; j++) {
            rc_mstr_append_char(&m, s.data[rc_array_u32_get(&idx, j)], arena);
        }
    }
    else if (value_is_list(index)) {
        for (uint32_t j = 0; j < index.list.num; j++) {
            uint32_t i = selector_index(rc_view_value_get(index.list, j), s.len);

            if (i == RC_INDEX_NONE) {
                return value_make_error(error_type_subscript_range);
            }

            rc_mstr_append_char(&m, s.data[i], arena);
        }
    }
    else {
        return value_make_error(error_type_type_mismatch);   // selector not int/range/list
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
        return value_make_error(error_type_type_mismatch);   // a number is not subscriptable
    }

    value sel = rc_view_value_get(indices, 0);
    rc_view_value rest = rc_view_value_get_tail(indices, 1);
    uint32_t len = v.list.num;

    if (value_is_number(sel)) {   // an integer drops this axis
        uint32_t i = selector_index(sel, len);
        if (i == RC_INDEX_NONE) {
            return value_make_error(error_type_subscript_range);
        }

        return subscript(rc_view_value_get(v.list, i), rest, arena);
    }

    // a range or list keeps this axis: recurse on each selected element into a result list
    rc_array_value out = {0};
    if (value_is_range(sel)) {
        rc_array_u32 idx = range_indices(sel.range, len, arena);
        for (uint32_t j = 0; j < idx.num; j++) {
            value e = subscript(rc_view_value_get(v.list, rc_array_u32_get(&idx, j)), rest, arena);
            rc_array_value_push(&out, e, arena);
        }
    }
    else if (value_is_list(sel)) {
        for (uint32_t j = 0; j < sel.list.num; j++) {
            uint32_t i = selector_index(rc_view_value_get(sel.list, j), len);

            if (i == RC_INDEX_NONE) {
                return value_make_error(error_type_subscript_range);
            }

            value e = subscript(rc_view_value_get(v.list, i), rest, arena);
            rc_array_value_push(&out, e, arena);
        }
    }
    else {
        return value_make_error(error_type_type_mismatch);   // selector not int/range/list
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
        return value_make_error(error_type_domain);   // an empty reduction with no identity
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
        return value_make_error(error_type_subscript_range);   // axis out of range for this branch
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
    }
    else if (value_is_range(v)) {
        flatten_into(range_to_list(v.range, arena), out, arena);
    }
    else {
        rc_array_value_push(out, v, arena);
    }
}

// The shared reduction body: f(L) folds every leaf to a scalar; f(L, axis) collapses one
// axis. op + identity pick the specific reduction (sum / product / min / max).
static value reduce(rc_view_value args, value (*op)(value, value, rc_arena *), value identity, rc_arena *arena)
{
    if (args.num < 1 || args.num > 2) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }

    if (value_is_range(v)) {
        v = range_to_list(v.range, arena);
        if (value_is_error(v)) {
            return v;
        }
    }

    if (args.num == 1) {
        rc_array_value leaves = {0};
        flatten_into(v, &leaves, arena);
        return fold(leaves.view, op, identity, arena);
    }

    uint32_t axis = selector_index(rc_view_value_get(args, 1), rank_of(v));   // 0 <= axis < rank
    if (axis == RC_INDEX_NONE) {
        return value_make_error(error_type_subscript_range);
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
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }

    if (value_is_string(v)) {
        return value_make_numeric(v.string.len);
    }

    if (value_is_list(v)) {
        return value_make_numeric(v.list.num);
    }

    if (value_is_range(v)) {
        value l = range_to_list(v.range, arena);
        return value_is_error(l) ? l : value_make_numeric(l.list.num);
    }

    return value_make_error(error_type_type_mismatch);   // a scalar has no length
}

// rank: the number of axes (0 for a scalar). A range is one axis whether or not its ends are
// bounded - rank needs no length, so even 2.. answers 1.
static value fn_rank(rc_view_value args, rc_arena *arena)
{
    (void)arena;

    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    return value_make_numeric(value_is_range(v) ? 1.0 : (double) rank_of(v));
}

// full(count, value): a list of `count` copies of value (NumPy's full(shape, fill_value)). value may be
// anything - full(3, {1,2}) is three copies of the pair. Handy on its own (EQUB full(16, &FF) fills a run),
// and the way to make N uniform draws: RND(full(N, 256)).
static value fn_full(rc_view_value args, rc_arena *arena)
{
    if (args.num != 2) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value count = rc_view_value_get(args, 0);
    value fill  = rc_view_value_get(args, 1);
    if (value_is_error(count)) {
        return count;
    }
    if (value_is_error(fill)) {
        return fill;                                     // propagate a forward reference / eval error
    }
    if (!value_is_number(count)) {
        return value_make_error(error_type_type_mismatch);
    }

    int64_t n = (int64_t)count.numeric;                  // truncate toward zero
    if (n < 0) {
        return value_make_error(error_type_domain);
    }
    if (n > VALUE_LIST_MAX_LENGTH) {
        return value_make_error(error_type_list_too_big);   // the same cap ranges use
    }

    rc_array_value out = rc_array_value_make((uint32_t)n, arena);
    for (int64_t i = 0; i < n; i++) {
        rc_array_value_push(&out, fill, arena);   // the same value handle n times; deep-copied on promote / emit
    }
    return value_make_list(out.view);
}

// flatten: every leaf, in order, as a single rank-1 list.
static value fn_flatten(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
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
            if (value_is_error(a)) {
                return a;
            }
        }

        if (value_is_list(a)) {
            for (uint32_t j = 0; j < a.list.num; j++) {
                rc_array_value_push(&out, rc_view_value_get(a.list, j), arena);
            }
        }
        else {
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
            if (value_is_error(a)) {
                return a;
            }
        }

        if (!value_is_list(a)) {
            return value_make_error(error_type_type_mismatch);
        }

        if (i == 0) {
            n = a.list.num;
        }
        else if (a.list.num != n) {
            return value_make_error(error_type_shape_mismatch);   // lengths must match
        }

        rc_array_value_push(&lists, a, arena);
    }

    rc_array_value out = {0};
    for (uint32_t i = 0; i < n; i++) {
        rc_array_value tuple = {0};
        for (uint32_t j = 0; j < lists.num; j++) {
            value lst = rc_view_value_get(lists.view, j);
            rc_array_value_push(
                &tuple,
                rc_view_value_get(lst.list, i),
                arena
            );
        }

        rc_array_value_push(
            &out,
            value_make_list(tuple.view),
            arena
        );
    }

    return value_make_list(out.view);
}

// reverse: the outermost axis reversed (a list's elements, or a string's characters).
static value fn_reverse(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
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

    return value_make_error(error_type_type_mismatch);
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
        return value_make_error(error_type_incorrect_parameters);
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
        return value_make_error(error_type_type_mismatch);
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

        if (!value_is_number(key)) {
            return value_make_error(error_type_type_mismatch);   // the key must be a number
        }

        rc_array_sort_pair_push(
            &pairs,
            (sort_pair) {
                .key = key.numeric,
                .v = e
            },
            arena
        );
    }
    sort_pairs(pairs.span);

    rc_array_value out = {0};
    for (uint32_t i = 0; i < n; i++) {
        rc_array_value_push(&out, rc_array_sort_pair_get(&pairs, i).v, arena);
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
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    bool unresolved = value_is_error(v) && v.error.code == error_type_unknown_symbol;
    return value_make_bool(!unresolved);
}

// chr: the inverse of codes - every numeric leaf of the argument (flattened, ranges enumerated)
// becomes one character of a single string, so chr(72) is "H", chr({72, 73}) is "HI", and
// chr(codes(s)) is s again. Doubling as the "join a list of codes into a string" the language
// otherwise lacks is the point of collapsing the shape. Fractions truncate toward zero like
// every other byte-sized context; a code outside 0..255 has no character to become.
static value fn_chr(rc_view_value args, rc_arena *arena)
{
    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }

    rc_array_value leaves = {0};
    flatten_into(v, &leaves, arena);

    rc_mstr m = rc_mstr_make(leaves.num, arena);
    for (uint32_t i = 0; i < leaves.num; i++) {
        value e = rc_view_value_get(leaves.view, i);
        if (value_is_error(e)) {
            return e;   // an unbounded / oversized range arrives from the flatten as an error leaf
        }
        if (!value_is_number(e)) {
            return value_make_error(error_type_type_mismatch);
        }
        double code = trunc(e.numeric);
        if (code < 0.0 || code > 255.0) {
            return value_make_error(error_type_domain);
        }
        rc_mstr_append_char(&m, (char) (uint8_t) code, arena);
    }
    return value_make_string(m.view);
}

// Append v the way PRINT shows it: a string raw (no quotes - the text IS the message),
// everything else in value_format's diagnostic shape. Shared by error() and find()'s payload.
static void append_value_raw(rc_mstr *out, value v, rc_arena *arena)
{
    if (value_is_string(v)) {
        rc_mstr_append(out, v.string, arena);
    }
    else {
        value_format(out, v, arena);
    }
}

// A whole haystack scanned and the needle nowhere in it: a not_found error naming the culprit,
// so "Not found: 'Q'" points straight at the character missing from the charset.
static value not_found(value needle, rc_arena *arena)
{
    rc_mstr m = rc_mstr_make(16, arena);
    append_value_raw(&m, needle, arena);
    return value_make_error_detail(error_type_not_found, m.view);
}

// One find: the zero-based index of needle's first occurrence in hay (already coerced to a
// list or a string). A compound needle broadcasts - find(from, codes(s)) is a same-shape list
// of indices, which is what makes table[find(from, codes(s))] the whole character map. A miss
// anywhere fails the whole call rather than embedding an error element: a gathered subscript
// would only garble it into "subscript out of range", a long way from the real complaint.
static value find_one(value hay, value needle, rc_arena *arena)
{
    if (value_is_error(needle)) {
        return needle;
    }

    if (value_is_range(needle)) {
        needle = range_to_list(needle.range, arena);
        if (value_is_error(needle)) {
            return needle;
        }
    }
    if (value_is_list(needle)) {
        rc_array_value out = rc_array_value_make(needle.list.num, arena);
        for (uint32_t i = 0; i < needle.list.num; i++) {
            value r = find_one(hay, rc_view_value_get(needle.list, i), arena);
            if (value_is_error(r)) {
                return r;
            }
            rc_array_value_push(&out, r, arena);
        }
        return value_make_list(out.view);
    }

    if (value_is_string(hay)) {
        // Substring search (so we can also answer "where does this token start"): the needle
        // must itself be a string; an empty one matches at the start.
        if (!value_is_string(needle)) {
            return value_make_error(error_type_type_mismatch);
        }
        if (needle.string.len <= hay.string.len) {
            for (uint32_t i = 0; i + needle.string.len <= hay.string.len; i++) {
                if (rc_str_is_equal(rc_str_substr(hay.string, i, needle.string.len), needle.string)) {
                    return value_make_numeric((double) i);
                }
            }
        }
        return not_found(needle, arena);
    }

    for (uint32_t i = 0; i < hay.list.num; i++) {
        if (value_is_equal(rc_view_value_get(hay.list, i), needle)) {
            return value_make_numeric((double) i);
        }
    }
    return not_found(needle, arena);
}

// find(haystack, needle): the index of needle's first occurrence in the haystack (a list, a
// range, or a string - a string haystack searches for a substring). The needle broadcasts;
// the haystack does not (it is the thing being searched, however deep its elements). Note the
// broadcast means a needle can never itself be a list-valued element of the haystack.
static value fn_find(rc_view_value args, rc_arena *arena)
{
    if (args.num != 2) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value hay = rc_view_value_get(args, 0);
    if (value_is_error(hay)) {
        return hay;
    }

    if (value_is_range(hay)) {
        hay = range_to_list(hay.range, arena);
        if (value_is_error(hay)) {
            return hay;
        }
    }
    if (!value_is_list(hay) && !value_is_string(hay)) {
        return value_make_error(error_type_type_mismatch);   // a scalar has nothing to search
    }

    return find_one(hay, rc_view_value_get(args, 1), arena);
}

// Whole-value type predicates: TRUE or FALSE for the value as a whole, deliberately NOT
// element-wise (a list is neither a string nor a number - list-ness is already spelled
// shape(x) != {}). Errors propagate as usual (only defined() inspects), so a forward
// reference still defers.
static value fn_is_string(rc_view_value args, rc_arena *arena)
{
    (void)arena;

    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    return value_make_bool(value_is_string(v));
}

static value fn_is_number(rc_view_value args, rc_arena *arena)
{
    (void)arena;

    if (args.num != 1) {
        return value_make_error(error_type_incorrect_parameters);
    }

    value v = rc_view_value_get(args, 0);
    if (value_is_error(v)) {
        return v;
    }
    // A ZA_AUTO address counts: it denotes the number it becomes at allocation, and answering
    // by its transient type would flip the answer between the settling and output passes.
    // A boolean counts too: it coerces to a number wherever one is wanted.
    return value_make_bool(value_is_number(v) || value_is_za_auto(v));
}

// error(...): the ERROR statement as a value. The arguments format PRINT-style (strings raw,
// everything else in value_format's shape, concatenated) into a user_error the diagnostics
// render verbatim, so a FUNCTION body can refuse bad input from behind an IF:
// r = error("bad width: ", w). An error argument propagates first, so a forward reference
// defers rather than firing prematurely.
static value fn_error(rc_view_value args, rc_arena *arena)
{
    for (uint32_t i = 0; i < args.num; i++) {
        value a = rc_view_value_get(args, i);
        if (value_is_error(a)) {
            return a;
        }
    }

    rc_mstr m = rc_mstr_make(16, arena);
    for (uint32_t i = 0; i < args.num; i++) {
        append_value_raw(&m, rc_view_value_get(args, i), arena);
    }
    return value_make_error_detail(error_type_user_error, m.view);
}


// ---- named constants ----
// Each hands back its value given the evaluation environment. The pure ones ignore it; const_pc reads the
// live PC. TRUE and FALSE are proper booleans, coercing to 1 / 0 wherever a number is wanted.
static value const_true(const expr_env *env)  { (void) env; return value_make_bool(true); }
static value const_false(const expr_env *env) { (void) env; return value_make_bool(false); }
static value const_pi(const expr_env *env)    { (void) env; return value_make_numeric(3.14159265358979323846); }
static value const_pc(const expr_env *env)    { return value_make_numeric((double) env->pc); }

// The local labels: @- is the nearest '.@' before this reference, @+ the nearest after, resolved against the
// current scope by source position (see scopes_find_local_label). An unresolved one comes back as an
// unknown-symbol error, so a forward @+ defers across passes like any forward reference.
static value const_prev_local(const expr_env *env) { return scopes_find_local_label(env->scopes, env->scope_index, env->source, env->offset, false); }
static value const_next_local(const expr_env *env) { return scopes_find_local_label(env->scopes, env->scope_index, env->source, env->offset, true ); }


// ---- the two context tables ----
// EVEN: lexed where an operand is expected (the start, after a binary op, after an
// open paren). Numbers/strings/identifiers come from the lexer itself, so the table
// only carries the leading operators, the functions, the constants, and the open paren.
static const token even_entries[] = {
    {RC_STR_INIT("("),     {.type = lexeme_type_open_paren}},
    {RC_STR_INIT("{"),     {.type = lexeme_type_open_brace}},            // begins a list literal
    {RC_STR_INIT("}"),     {.type = lexeme_type_close_brace}},           // ends one (empty, or after a comma)

    {RC_STR_INIT(".."),    {.type = lexeme_type_range}},                 // start-unbounded range
    {RC_STR_INIT("..<"),   {.type = lexeme_type_range, .range = {.exclusive = true}}},

    {RC_STR_INIT("+"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = op_pos, .precedence = prec_neg}}},
    {RC_STR_INIT("-"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = op_neg, .precedence = prec_neg}}},

    // Bare low/high-byte operators (6502 style): '<' is the low byte, '>' the high byte. Very
    // low precedence, so they swallow the whole following expression: <start+1 is lo(start+1).
    {RC_STR_INIT("<"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_lo, .precedence = prec_lohi}}},
    {RC_STR_INIT(">"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_hi, .precedence = prec_lohi}}},

    {RC_STR_INIT("~"),     {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_hex, .precedence = prec_lohi}}},

    // Element-wise builtins are parenthesised unary ops: the '(' is part of the token (so
    // the name only reads as a call when followed by '(' - 'lo' is a variable, 'lo(' the op),
    // and precedence is unused (left default) because the argument is closed by ')'.
    {RC_STR_INIT("abs("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_abs}}},
    {RC_STR_INIT("lo("),    {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_lo}}},
    {RC_STR_INIT("hi("),    {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_hi}}},
    {RC_STR_INIT("sqrt("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_sqrt}}},
    {RC_STR_INIT("rnd("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_rnd}}},   // one random in [0,n); broadcasts
    {RC_STR_INIT("not("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_not}}},
    {RC_STR_INIT("int("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_int}}},
    {RC_STR_INIT("floor("), {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_int}}},
    {RC_STR_INIT("round("), {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_round}}},
    {RC_STR_INIT("trunc("), {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_trunc}}},
    {RC_STR_INIT("ceil("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_ceil}}},
    {RC_STR_INIT("sin("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_sin}}},
    {RC_STR_INIT("cos("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_cos}}},
    {RC_STR_INIT("tan("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_tan}}},
    {RC_STR_INIT("asin("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_asin}}},
    {RC_STR_INIT("acos("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_acos}}},
    {RC_STR_INIT("atan("),  {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_atan}}},
    {RC_STR_INIT("log("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_log}}},     // base 10
    {RC_STR_INIT("ln("),    {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_ln}}},      // natural
    {RC_STR_INIT("exp("),   {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_exp}}},
    {RC_STR_INIT("codes("), {.type = lexeme_type_unary_op, .unary_op = {.apply = fn_codes}}},   // string -> list of char codes

    // Structural/variadic builtins are functions: the handler gets the whole arg list.
    {RC_STR_INIT("shape("),   {.type = lexeme_type_function, .function = {.apply = fn_shape}}},
    {RC_STR_INIT("len("),     {.type = lexeme_type_function, .function = {.apply = fn_len}}},
    {RC_STR_INIT("rank("),    {.type = lexeme_type_function, .function = {.apply = fn_rank}}},
    {RC_STR_INIT("full("),    {.type = lexeme_type_function, .function = {.apply = fn_full}}},
    {RC_STR_INIT("repeated("), {.type = lexeme_type_function, .function = {.apply = fn_full}}},   // full by a friendlier name
    {RC_STR_INIT("flatten("), {.type = lexeme_type_function, .function = {.apply = fn_flatten}}},
    {RC_STR_INIT("concat("),  {.type = lexeme_type_function, .function = {.apply = fn_concat}}},
    {RC_STR_INIT("zip("),      {.type = lexeme_type_function, .function = {.apply = fn_zip}}},
    {RC_STR_INIT("reverse("), {.type = lexeme_type_function, .function = {.apply = fn_reverse}}},
    {RC_STR_INIT("sort("),    {.type = lexeme_type_function, .function = {.apply = fn_sort}}},
    {RC_STR_INIT("sum("),     {.type = lexeme_type_function, .function = {.apply = fn_sum}}},
    {RC_STR_INIT("product("), {.type = lexeme_type_function, .function = {.apply = fn_product}}},
    {RC_STR_INIT("min("),     {.type = lexeme_type_function, .function = {.apply = fn_min}}},
    {RC_STR_INIT("max("),     {.type = lexeme_type_function, .function = {.apply = fn_max}}},
    {RC_STR_INIT("defined("), {.type = lexeme_type_function, .function = {.apply = fn_defined}}},
    {RC_STR_INIT("chr("),     {.type = lexeme_type_function, .function = {.apply = fn_chr}}},
    {RC_STR_INIT("find("),    {.type = lexeme_type_function, .function = {.apply = fn_find}}},
    {RC_STR_INIT("is_string("), {.type = lexeme_type_function, .function = {.apply = fn_is_string}}},
    {RC_STR_INIT("is_number("), {.type = lexeme_type_function, .function = {.apply = fn_is_number}}},
    {RC_STR_INIT("error("),   {.type = lexeme_type_function, .function = {.apply = fn_error}}},

    // Named constants. The bare words behave like the word operators (div / mod / and): a longer identifier
    // still wins, so PI vs PICKLE. The PC constant is '*' in operand position (multiply lives in the odd
    // table, so there is no clash), with P% as the BBC Micro spelling.
    {RC_STR_INIT("true"),  {.type = lexeme_type_constant, .constant = {.handle = const_true}}},
    {RC_STR_INIT("false"), {.type = lexeme_type_constant, .constant = {.handle = const_false}}},
    {RC_STR_INIT("pi"),    {.type = lexeme_type_constant, .constant = {.handle = const_pi}}},
    {RC_STR_INIT("*"),     {.type = lexeme_type_constant, .constant = {.handle = const_pc}}},
    {RC_STR_INIT("P%"),    {.type = lexeme_type_constant, .constant = {.handle = const_pc}}},

    // Local-label references. '@' is not an identifier character, so these two-char tokens never collide with
    // a name; longest-prefix matching takes '@-' / '@+' whole.
    {RC_STR_INIT("@-"),    {.type = lexeme_type_constant, .constant = {.handle = const_prev_local}}},
    {RC_STR_INIT("@+"),    {.type = lexeme_type_constant, .constant = {.handle = const_next_local}}},
};

// ODD: lexed where a binary operator is expected (after an operand). The close paren
// lives here, so a parenthesised group is closed from operator position.
static const token odd_entries[] = {
    {RC_STR_INIT(")"),   {.type = lexeme_type_close_paren}},
    {RC_STR_INIT("}"),   {.type = lexeme_type_close_brace}},         // ends a list (after an element)
    {RC_STR_INIT("["),   {.type = lexeme_type_open_bracket}},        // postfix subscript
    {RC_STR_INIT("]"),   {.type = lexeme_type_close_bracket}},       // ends a subscript

    {RC_STR_INIT(".."),  {.type = lexeme_type_range}},               // range operator (handled specially)
    {RC_STR_INIT("..<"), {.type = lexeme_type_range, .range = {.exclusive = true}}},

    {RC_STR_INIT("^"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_pow,  .precedence = prec_pow, .associativity = assoc_right}}},
    {RC_STR_INIT("*"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_mul,  .precedence = prec_mul}}},
    {RC_STR_INIT("/"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_div,  .precedence = prec_mul}}},
    {RC_STR_INIT("div"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_idiv, .precedence = prec_mul}}},
    {RC_STR_INIT("mod"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_mod,  .precedence = prec_mul}}},
    {RC_STR_INIT("<<"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_shl,  .precedence = prec_mul}}},
    {RC_STR_INIT(">>"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_shr,  .precedence = prec_mul}}},
    {RC_STR_INIT("+"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_add,  .precedence = prec_add}}},
    {RC_STR_INIT("-"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_sub,  .precedence = prec_add}}},
    {RC_STR_INIT("="),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_eq,   .precedence = prec_cmp}}},
    {RC_STR_INIT("=="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_eq,   .precedence = prec_cmp}}},
    {RC_STR_INIT("!="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_ne,   .precedence = prec_cmp}}},
    {RC_STR_INIT("<>"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_ne,   .precedence = prec_cmp}}},
    {RC_STR_INIT("<="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_le,   .precedence = prec_cmp}}},
    {RC_STR_INIT(">="),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_ge,   .precedence = prec_cmp}}},
    {RC_STR_INIT("<"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_lt,   .precedence = prec_cmp}}},
    {RC_STR_INIT(">"),   {.type = lexeme_type_binary_op, .binary_op = {.apply = op_gt,   .precedence = prec_cmp}}},
    {RC_STR_INIT("and"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_and,  .precedence = prec_and}}},
    {RC_STR_INIT("or"),  {.type = lexeme_type_binary_op, .binary_op = {.apply = op_or,   .precedence = prec_or}}},
    {RC_STR_INIT("eor"), {.type = lexeme_type_binary_op, .binary_op = {.apply = op_eor,  .precedence = prec_or}}},
};

static const token_table even_tokens = RC_VIEW(even_entries);
static const token_table odd_tokens  = RC_VIEW(odd_entries);

token_table expression_operand_base(void) { return even_tokens; }

// The operand table to lex from: the dynamic one the env carries (base + user-FUNCTION names) when present,
// else the static base. A bare env (expression.c's own tests) leaves operand_tokens {0} and gets the base.
static token_table operand_table(const parser *p) {
    return p->env->operand_tokens.num ? p->env->operand_tokens : even_tokens;
}

// The FUNCTION-body statement keywords - the minimum a body needs. Its own tiny table, so a body can lex only
// these (plus intrinsic identifiers/terminators): an opcode or EQUB name is just an identifier here, an
// assignment target, never code. The '=' return marker reuses lexeme_type_assign (statement-start position).
typedef enum body_keyword { body_if, body_elif, body_else, body_endif } body_keyword;
static const token function_body_entries[] = {
    {RC_STR_INIT("if"),    {.type = lexeme_type_closer, .closer = {body_if,    error_type_none}}},
    {RC_STR_INIT("elif"),  {.type = lexeme_type_closer, .closer = {body_elif,  error_type_none}}},
    {RC_STR_INIT("else"),  {.type = lexeme_type_closer, .closer = {body_else,  error_type_none}}},
    {RC_STR_INIT("endif"), {.type = lexeme_type_closer, .closer = {body_endif, error_type_none}}},
    {RC_STR_INIT("="),     {.type = lexeme_type_assign}},
};
static const token_table function_body_tokens = RC_VIEW(function_body_entries);


// Require a ')' at pos (lexed from operator position). On success returns v with
// the pos past the ')'; otherwise an expected_close_paren error.
static expr_result expect_close_paren(const parser *p, value v, uint32_t pos)
{
    lexer_result rp = lexer_next(p->text, pos, odd_tokens);
    if (rp.token.type != lexeme_type_close_paren) {
        return fail(expr_error_expected_close_paren, pos);
    }
    return ok(v, rp.next);
}

// parse_operand and parse_precedence are mutually recursive, so one of the pair must
// be declared ahead; everything else below is defined in call order (callees first).
static expr_result parse_precedence(const parser *p, uint32_t pos, uint8_t min_prec);

// Accept a newline if one is here (a list literal treats newlines as whitespace),
// returning the pos past it, else the pos unchanged. The lexer coalesces a run
// of newlines into one terminator, so there is only ever one to skip. We lex with `tt`
// so the caller can re-lex the same spot for whatever it expects there; a ':' or EOF
// lexes as a hard terminator and is left in place, so an unclosed list is reported.
static uint32_t accept_newline(const parser *p, uint32_t pos, token_table tt)
{
    lexer_result lr = lexer_next(p->text, pos, tt);
    if (lr.token.type == lexeme_type_terminator && lr.token.terminator.newline) {
        return lr.next;
    }
    return pos;
}

// Parse a list literal from just after the '{': comma-separated element expressions
// (nested lists allowed, empty allowed), with newlines ignored inside the braces. We
// gather the elements in the parser's scratch arena and wrap that view - no copy,
// since a value is a non-owning handle.
static expr_result parse_list(const parser *p, uint32_t pos)
{
    rc_array_value elems = {0};

    while (true) {
        // Value-or-'}' position.
        pos = accept_newline(p, pos, operand_table(p));

        lexer_result lr = lexer_next(p->text, pos, operand_table(p));
        if (lr.token.type == lexeme_type_close_brace) {
            return ok(value_make_list(elems.view), lr.next);   // possibly empty
        }

        // One element, a full expression. A soft fail here (e.g. "{,}") is a real error.
        expr_result e = parse_precedence(p, pos, 0);
        if (e.error != expr_error_none) {
            return e;
        }

        rc_array_value_push(&elems, e.value, p->arena);
        pos = e.next;

        // Separator position: after any newline we want a ',' or '}'.
        pos = accept_newline(p, pos, odd_tokens);
        lr = lexer_next(p->text, pos, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;   // a trailing comma simply loops back and finds the '}'
        }

        if (lr.token.type == lexeme_type_close_brace) {
            return ok(value_make_list(elems.view), lr.next);
        }
        
        return fail(expr_error_expected_close_brace, pos);
    }
}

// Collect a call's arguments from just after the '(' (which was part of the token): comma-separated
// expressions up to the ')', pushed into *args (the parser's scratch arena). On success returns .next just
// past the ')' (its .value is unused); a soft expected_expression (e.g. "f(1,)") or a missing ')' is a real
// error. Shared by builtin functions and user-defined FUNCTION calls.
static expr_result collect_args(const parser *p, uint32_t pos, rc_array_value *args)
{
    RC_ASSERT(pos > 0 && p->text.data[pos - 1] == '(');   // the '(' is part of the call token

    // An immediate ')' is an empty argument list.
    lexer_result lr = lexer_next(p->text, pos, odd_tokens);
    if (lr.token.type == lexeme_type_close_paren) {
        return ok(value_make_none(), lr.next);
    }

    while (true) {
        expr_result a = parse_precedence(p, pos, 0);
        if (a.error != expr_error_none) {
            return a;
        }

        rc_array_value_push(args, a.value, p->arena);
        pos = a.next;

        lr = lexer_next(p->text, pos, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;
        }

        if (lr.token.type == lexeme_type_close_paren) {
            return ok(value_make_none(), lr.next);
        }

        return fail(expr_error_expected_close_paren, pos);
    }
}

// A builtin (variadic / structural) function call: collect args, then hand the raw list to its handler,
// which validates count/types and decides how to treat error operands (most propagate, defined() inspects).
static expr_result parse_call_args(const parser *p, lexeme_function fn, uint32_t pos)
{
    rc_array_value args = {0};
    expr_result r = collect_args(p, pos, &args);
    if (r.error != expr_error_none) {
        return r;
    }
    return ok(fn.apply(args.view, p->arena), r.next);
}

// ---- user-defined FUNCTION calls: interpreted here, in the evaluator, with no callback into the assembler ----

#define FUNCTION_MAX_DEPTH 64u   // a runaway backstop; a bounded recursion unwinds well before it

// Where a body statement-run stopped.
typedef enum body_stop {
    body_stop_return,   // a top-level '=' return
    body_stop_elif,     // an ELIF closer (left AT the keyword for the caller)
    body_stop_else,     // an ELSE closer
    body_stop_endif,    // an ENDIF closer
    body_stop_eof,      // end of the body source
} body_stop;

typedef struct body_result {
    uint8_t  stop;           // body_stop
    uint32_t   next;         // past the return expression, or AT the elif/else/endif keyword, or at EOF
    value      value;        // the return value (stop == body_stop_return); value_make_none() = empty return
    bool       saw_statement;// did any assignment / IF run before the stop (to tell a forward decl from a body)
    uint16_t error;          // error_type
    uint32_t   error_at;
    rc_str     error_detail; // a payload for the error, when one helps (the duplicated local's name)
} body_result;

static body_result interpret_statements(const parser *p, uint32_t pos, bool active);

static body_result body_fail(error_type code, uint32_t at)
{
    return (body_result) {
        .next     = at,
        .error    = code,
        .error_at = at,
    };
}

// A single-token '=' table, to read the assignment operator after a body statement's target name.
static const token assign_only_entries[] = { {RC_STR_INIT("="), {.type = lexeme_type_assign}} };
static const token_table assign_only_tokens = RC_VIEW(assign_only_entries);

// A sub-parser for a body expression (a condition, an assignment RHS, the return). It inherits everything
// but forces user-FUNCTION calls dead in an inactive branch: a dead branch is still PARSED (to find where
// it ends), so any call it contains must short-circuit rather than run - otherwise a dead branch's recursion
// would fire on every step, running to the depth cap and exhausting the scratch arena.
static parser body_sub(const parser *p, bool active)
{
    parser sp = *p;
    sp.live = p->live && active;
    return sp;
}

// A body assignment `name = expr`, from just after `name` (whose start is `name_at`). A single immutable
// binding into the current (child) scope when active; when inactive, clear only the binding this statement
// owns (the dead-branch rule). Returns the cursor after the RHS.
static body_result interpret_assignment(const parser *p, rc_str name, uint32_t pos, bool active, uint32_t name_at)
{
    lexer_result eq = lexer_next(p->text, pos, assign_only_tokens);
    if (eq.token.type != lexeme_type_assign) {
        return body_fail(error_type_expected_assign, pos);
    }

    parser sp = body_sub(p, active);
    expr_result rhs = parse_precedence(&sp, eq.next, 0);
    if (rhs.error != expr_error_none) {
        return body_fail(error_type_expression, rhs.error_at);
    }

    cursor at = {p->env->source, name_at};
    if (active) {
        // Bindings are single-assignment: a second live assignment to the same name in one call
        // (a parameter included) would otherwise be silently ignored, which reads like mutation
        // that never happens. Branches are fine - only the live one binds.
        if (scopes_set_symbol(p->env->scopes, p->env->scope_index, name, rhs.value, at) == symbol_status_duplicate) {
            body_result dup = body_fail(error_type_duplicate_symbol, name_at);
            dup.error_detail = name;
            return dup;
        }
    }
    else if (cursor_is_equal(scopes_symbol_def(p->env->scopes, p->env->scope_index, name), at)) {
        scopes_remove_symbol(p->env->scopes, p->env->scope_index, name);
    }

    return (body_result) {.next = rhs.next, .saw_statement = true};
}

// An IF ... [ELIF ...] [ELSE ...] ENDIF, from just after the `if`/`elif` keyword. Runs the selected branch
// live and scans the rest; nested IFs recurse. Returns the cursor past ENDIF. Return (`=`) is top-level only,
// so a return met inside a branch is an error.
static body_result interpret_if(const parser *p, uint32_t pos, bool active)
{
    parser sp = body_sub(p, active);
    expr_result cond = parse_precedence(&sp, pos, 0);
    if (cond.error != expr_error_none) {
        return body_fail(error_type_expression, cond.error_at);
    }

    // A live IF with a KNOWN condition (a number or a boolean) picks a branch; an unknown /
    // non-numeric condition is undecidable, so no branch runs and the eventual return defers
    // (resolves on a later pass).
    bool decided     = active && value_is_number(cond.value);
    bool run_if      = decided && cond.value.numeric != 0.0;
    bool else_active = decided && !run_if;

    body_result br = interpret_statements(p, cond.next, run_if);
    if (br.error != error_type_none) {
        return br;
    }
    if (br.stop == body_stop_return) {
        return body_fail(error_type_unclosed_function, br.next);   // a return inside a branch: top-level only
    }
    if (br.stop == body_stop_eof) {
        return body_fail(error_type_unclosed_function, br.next);   // IF with no ENDIF
    }

    // Consume the closer keyword the branch stopped at.
    lexer_result closer = lexer_next(p->text, br.next, function_body_tokens);
    if (br.stop == body_stop_elif) {
        return interpret_if(p, closer.next, else_active);   // the ELIF is a fresh IF over the else path
    }
    if (br.stop == body_stop_else) {
        body_result eb = interpret_statements(p, closer.next, else_active);
        if (eb.error != error_type_none) {
            return eb;
        }
        if (eb.stop != body_stop_endif) {
            return body_fail(error_type_unclosed_function, eb.next);   // ELSE must end at ENDIF
        }
        lexer_result end = lexer_next(p->text, eb.next, function_body_tokens);
        return (body_result) {.next = end.next, .saw_statement = true};
    }
    // body_stop_endif
    return (body_result) {.next = closer.next, .saw_statement = true};
}

// Run body statements (assignments, IFs) until the top-level '=' return, a branch closer (elif/else/endif),
// or EOF. `active` gates whether assignments bind and IF branches execute (an inactive run just walks the
// structure, for the definition scan and dead branches).
static body_result interpret_statements(const parser *p, uint32_t pos, bool active)
{
    bool saw = false;
    while (true) {
        lexer_result lr = lexer_next(p->text, pos, function_body_tokens);

        if (lr.token.type == lexeme_type_assign) {           // '=' return
            parser sp = body_sub(p, active);
            expr_result rhs = parse_precedence(&sp, lr.next, 0);
            if (rhs.error == expr_error_expected_expression) {
                return (body_result) {
                    .stop          = body_stop_return,
                    .next          = lr.next,
                    .value         = value_make_none(),   // empty (a forward declaration)
                    .saw_statement = saw,
                };
            }
            if (rhs.error != expr_error_none) {
                return body_fail(error_type_expression, rhs.error_at);
            }
            return (body_result) {
                .stop          = body_stop_return,
                .next          = rhs.next,
                .value         = rhs.value,
                .saw_statement = saw,
            };
        }

        if (lr.token.type == lexeme_type_closer) {
            switch (lr.token.closer.id) {
                case body_if: {
                    body_result ifr = interpret_if(p, lr.next, active);
                    if (ifr.error != error_type_none) {
                        return ifr;
                    }
                    pos = ifr.next;
                    saw = true;
                    continue;
                }
                case body_elif: return (body_result) {.stop = body_stop_elif,  .next = pos, .saw_statement = saw};
                case body_else: return (body_result) {.stop = body_stop_else,  .next = pos, .saw_statement = saw};
                default:        return (body_result) {.stop = body_stop_endif, .next = pos, .saw_statement = saw};
            }
        }

        if (lr.token.type == lexeme_type_identifier) {
            body_result a = interpret_assignment(p, lr.token.identifier.name, lr.next, active, pos);
            if (a.error != error_type_none) {
                return a;
            }
            pos = a.next;
            saw = true;
            continue;
        }

        if (lr.token.type == lexeme_type_terminator) {
            if (lexer_at_end(p->text, lr.next)) {
                return (body_result) {
                    .stop          = body_stop_eof,
                    .next          = lr.next,
                    .saw_statement = saw,
                };
            }
            pos = lr.next;   // a blank statement
            continue;
        }

        return body_fail(error_type_unexpected_token, pos);   // nothing else is a legal body statement
    }
}

// A user-defined FUNCTION call in operand position. `call_pos` is the token's start in the CALLER source
// (for the child-scope key), `pos` is just past the '('. Collects arguments in the caller scope, matches an
// overload by arity, then interprets the body in a fresh child scope parented on the definition scope.
static expr_result interpret_call(const parser *p, uint32_t index, uint32_t call_pos, uint32_t pos, bool active)
{
    rc_array_value args = {0};
    expr_result r = collect_args(p, pos, &args);
    if (r.error != expr_error_none) {
        return r;
    }
    uint32_t after = r.next;   // past ')'

    const function_signature *sig = functions_match(p->env->functions, index, args.view.num);
    if (sig == NULL) {
        return ok(value_make_error(error_type_no_matching_arity), after);
    }
    if (!sig->defined) {
        return ok(value_make_error(error_type_function_not_defined), after);   // only forward-declared
    }

    // Inactive (the definition scan, or a dead enclosing branch): arguments consumed, body NOT run - which is
    // what stops the inactive scan from recursing into a self-call. Yield a deferrable placeholder.
    if (!active) {
        return ok(value_make_error(error_type_unknown_symbol), after);
    }

    if (p->env->call_depth && *p->env->call_depth >= FUNCTION_MAX_DEPTH) {
        return ok(value_make_error(error_type_function_too_deep), after);
    }

    // A per-call child scope, parented on the DEFINITION scope so lookup is LEXICAL: a body sees its own
    // params and whatever was in scope where it was defined (the globals at root, normally), never the
    // caller's locals. But the frame is KEYED on its caller's scope index (plus the call site): that is what
    // keeps two independent call chains from aliasing one shared frame. Keying on the call site AND depth
    // alone - under one flat def-scope parent - collided distinct chains that reached the same site+depth,
    // and corrupted deep or repeated recursion. The caller's scope index is unique per frame in the tree, so
    // it names the chain; call_pos separates several call sites that share one caller frame (qsort calls lt,
    // ge and qsort from one body). It is stable pass-to-pass (scopes are created in a deterministic order).
    char storage[80];
    rc_mstr key = {.data = storage, .len = 0, .cap = sizeof storage};
    rc_mstr_append_char(&key, '@', NULL);
    rc_mstr_append_u32(&key, p->env->scope_index, NULL);   // the caller frame - names this call chain
    rc_mstr_append_char(&key, ':', NULL);
    rc_mstr_append_u32(&key, call_pos, NULL);
    uint32_t child = scopes_get_or_make_child(p->env->scopes, sig->def_scope, key.view);

    cursor call_at = {p->env->source, call_pos};
    for (uint32_t i = 0; i < sig->params.num; i++) {
        scopes_set_symbol(p->env->scopes, child, rc_view_rc_str_get(sig->params, i),
                          rc_view_value_get(args.view, i), call_at);
    }

    // Interpret the body in the child scope, in ITS source (a body may live in a different file than the call).
    expr_env body_env = *p->env;
    body_env.scope_index = child;
    body_env.source      = sig->body.source;
    body_env.offset      = sig->body.pos;
    parser body_p = {.text = source_files_text(p->env->sources, sig->body.source), .env = &body_env,
                     .arena = p->arena, .live = true};

    if (p->env->call_depth) { (*p->env->call_depth)++; }
    body_result br = interpret_statements(&body_p, sig->body.pos, true);
    if (p->env->call_depth) { (*p->env->call_depth)--; }

    if (br.error != error_type_none) {
        return ok(value_make_error_detail(br.error, br.error_detail), after);   // a malformed body -> a value
    }
    if (br.stop != body_stop_return) {
        return ok(value_make_error(error_type_unclosed_function), after);
    }
    return ok(br.value, after);
}

function_body_scan expression_scan_function_body(rc_str text, uint32_t pos, const expr_env *env, rc_arena *arena)
{
    parser p = {   // scan: nested calls do not execute
        .text  = text,
        .env   = env,
        .arena = arena,
        .live  = false,
    };
    body_result br = interpret_statements(&p, pos, false);   // inactive: just walk to the top-level '='

    if (br.error != error_type_none) {
        return (function_body_scan) {
            .next     = br.error_at,
            .error    = br.error,
            .error_at = br.error_at,
        };
    }
    if (br.stop != body_stop_return) {
        return (function_body_scan) {
            .next     = br.next,
            .error    = error_type_unclosed_function,
            .error_at = br.next,
        };
    }

    bool empty_return = value_is_none(br.value);
    if (br.saw_statement && empty_return) {
        return (function_body_scan) {
            .next     = br.next,
            .error    = error_type_unclosed_function,
            .error_at = br.next,
        };
    }
    // A forward declaration is an empty body AND an empty return; anything with a real return is defined.
    return (function_body_scan) {.next = br.next, .defined = !empty_return};
}

// Parse one operand: a literal, a symbol, a parenthesised group, a prefixed unary
// expression, a function call, or a list literal. A lexeme that cannot begin an
// operand is a soft expected_expression failure, leaving the caller to decide.
static expr_result parse_operand(const parser *p, uint32_t pos)
{
    lexer_result lr = lexer_next(p->text, pos, operand_table(p));
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
            value v = scopes_get_symbol(p->env->scopes, p->env->scope_index, lex.identifier.name);
            // Not found is not a parse error: it becomes an error value that propagates, so a forward
            // reference can resolve on a later pass. This is the one place that still KNOWS the name, so
            // it rides along as the error's detail - "Undefined symbol: 'x'" gets its x from here.
            // (scopes_get_symbol itself yields a detail-less unknown for a dotted path that goes astray;
            // stamp the name on that too.)
            if (value_is_none(v) || (value_is_error(v) && v.error.code == error_type_unknown_symbol)) {
                v = value_make_error_detail(error_type_unknown_symbol, lex.identifier.name);
            }
            return ok(v, lr.next);
        }

        case lexeme_type_constant:
            return ok(lex.constant.handle(p->env), lr.next);

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

        case lexeme_type_user_function:
            // A user-defined FUNCTION: `pos` is the token start (the call site), `lr.next` is past its '('.
            // `p->live` is false only inside a definition scan, where a call must not actually execute.
            return interpret_call(p, lex.user_function.index, pos, lr.next, p->live);

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
            return fail(expr_error_expected_expression, pos);
    }
}

// Parse a subscript from just after the '[': comma-separated selector expressions up to
// the ']', then index target by them. Selectors are full expressions (so i, a..b, {..}
// and a bare .. all work, each stopping cleanly at the ',' or ']'). Newlines are not
// skipped - a subscript is an inline postfix. An empty '[]', a trailing comma, or a
// missing ']' is a committed error; index/type problems become propagating error values.
static expr_result parse_subscript(const parser *p, value target, uint32_t pos)
{
    rc_array_value indices = {0};

    while (true) {
        expr_result s = parse_precedence(p, pos, 0);
        if (s.error == expr_error_expected_expression) {
            return fail(expr_error_expected_expression, pos);   // empty "[]" or a trailing comma
        }

        if (s.error != expr_error_none) {
            return s;
        }

        rc_array_value_push(&indices, s.value, p->arena);
        pos = s.next;

        lexer_result lr = lexer_next(p->text, pos, odd_tokens);
        if (lr.token.type == lexeme_type_comma) {
            pos = lr.next;
            continue;
        }

        if (lr.token.type == lexeme_type_close_bracket) {
            return ok(subscript(target, indices.view, p->arena), lr.next);
        }

        return fail(expr_error_expected_close_bracket, pos);
    }
}

// Parse an expression whose operators bind at least as tightly as min_prec, folding
// left-to-right. Stops (greedily) at the first lexeme that is not a usable binary
// operator, returning the value so far and the pos before that lexeme.
static expr_result parse_precedence(const parser *p, uint32_t pos, uint8_t min_prec)
{
    expr_result lhs = parse_operand(p, pos);
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

expr_result expression_parse(rc_str text, uint32_t pos, const expr_env *env, rc_arena *arena)
{
    RC_ASSERT(rc_str_is_valid(text));
    RC_ASSERT(env != NULL && env->scopes != NULL);

    parser p = {
        .text = text,
        .env = env,
        .arena = arena,
        .live = true
    };

    return parse_precedence(&p, pos, 0);
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
    scopes_init(&fix->scopes, &fix->arena);
    scopes_make_root(&fix->scopes);
}

RC_TEST_GROUP_DEINIT(expression, fix)
{
    rc_arena_deinit(&fix->arena);
}

// Parse a source literal from offset 0 in the fixture's root scope.
#define RESULT(src) expression_parse(RC_STR(src), 0, &(expr_env){.scopes = &fix->scopes, .scope_index = 0, .pc = 0}, &fix->arena)
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

RC_TEST_STEP(expression, constants, fix)
{
    // Pure named constants evaluate to their value, case-insensitively.
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("FALSE"), value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("PI"),    value_make_numeric(3.14159265358979323846)));
    RC_CHECK_TRUE(value_is_equal(VAL("true"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("Pi"),    value_make_numeric(3.14159265358979323846)));
    // They compose like any other operand; two booleans get the logical AND.
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE and FALSE"), value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("2*PI"), value_make_numeric(2.0 * 3.14159265358979323846)));
    // A longer identifier still wins: PICKLE is a symbol (here unbound), not PI followed by CKLE.
    value pickle = VAL("PICKLE");
    RC_CHECK_TRUE(value_is_error(pickle) && pickle.error.code == error_type_unknown_symbol);
    // The PC constant reads env->pc (0 in this fixture); P% is its BBC Micro alias. Multiply still works,
    // because '*' as an operator is lexed from the odd table.
    RC_CHECK_TRUE(value_is_equal(VAL("*"),   value_make_numeric(0.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("P%"),  value_make_numeric(0.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("*+1"), value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("2*3"), value_make_numeric(6.0)));
}

RC_TEST_STEP(expression, random, fix)
{
    expression_reset_random();

    // RND(1) is always 0 - the only integer in [0,1) - regardless of the seed.
    RC_CHECK_TRUE(value_is_equal(VAL("RND(1)"), value_make_numeric(0)));
    // A scalar draw is a number in range.
    value r = VAL("RND(256)");
    RC_CHECK_TRUE(value_is_numeric(r) && r.numeric >= 0 && r.numeric < 256);

    // full builds a list of copies (REPEATED is the same function by a friendlier name)...
    value sevens[] = {value_make_numeric(7), value_make_numeric(7), value_make_numeric(7)};
    RC_CHECK_TRUE(value_is_equal(VAL("full(3, 7)"), value_make_list((rc_view_value) RC_VIEW(sevens))));
    RC_CHECK_TRUE(value_is_equal(VAL("REPEATED(3, 7)"), value_make_list((rc_view_value) RC_VIEW(sevens))));
    value empty = VAL("full(0, 9)");
    RC_CHECK_TRUE(value_is_list(empty) && empty.list.num == 0);

    // ...and RND broadcasts over it: N draws, each in range.
    value draws = VAL("RND(full(4, 256))");
    RC_CHECK_TRUE(value_is_list(draws) && draws.list.num == 4);
    for (uint32_t i = 0; i < draws.list.num; i++) {
        value e = rc_view_value_get(draws.list, i);
        RC_CHECK_TRUE(value_is_numeric(e) && e.numeric >= 0 && e.numeric < 256);
    }

    // Reproducible from a reset: the same seed replays the same draw.
    expression_reset_random();
    value a = VAL("RND(1000)");
    expression_reset_random();
    value b = VAL("RND(1000)");
    RC_CHECK_TRUE(value_is_equal(a, b));

    // Errors.
    RC_CHECK_TRUE(value_is_error(VAL("RND(0)")));        // no 0..n-1 range -> domain
    RC_CHECK_TRUE(value_is_error(VAL("full(-1, 0)")));   // negative count -> domain
    RC_CHECK_TRUE(value_is_error(VAL("full(1)")));       // one arg -> incorrect_parameters
}

RC_TEST_STEP(expression, unary, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("-2^2"), value_make_numeric(-4.0)));   // -(2^2)
    RC_CHECK_TRUE(value_is_equal(VAL("-2*3"), value_make_numeric(-6.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("+5"),   value_make_numeric(5.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("--5"),  value_make_numeric(5.0)));    // -(-5)

    // '!' is no longer an operator (NOT on a boolean does its old job); '!=' still means not-equal.
    RC_CHECK_TRUE(RESULT("!5").error == expr_error_expected_expression);
    RC_CHECK_TRUE(value_is_equal(VAL("3 != 3"), value_make_bool(false)));

    // NOT maps element-wise over a list, like any unary op: logical per boolean element.
    value mask[] = {value_make_bool(false), value_make_bool(true)};
    RC_CHECK_TRUE(value_is_equal(VAL("NOT({TRUE, FALSE})"), value_make_list((rc_view_value) RC_VIEW(mask))));
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
    scopes_set_symbol(&fix->scopes, 0, RC_STR("lo"), value_make_numeric(7.0), (cursor){0, 0});
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
    scopes_set_symbol(&fix->scopes, 0, RC_STR("foo"), value_make_numeric(42.0), (cursor){0, 0});

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

    // The same operators on two booleans are logical, yielding a boolean.
    RC_CHECK_TRUE(value_is_equal(VAL("NOT(TRUE)"),      value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("NOT(FALSE)"),     value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE or FALSE"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE and FALSE"), value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE eor TRUE"),  value_make_bool(false)));
    // A mixed boolean/number pair is refused - neither reading is honest.
    RC_CHECK_TRUE(value_is_error(VAL("1 and TRUE")));
    RC_CHECK_TRUE(value_is_error(VAL("FALSE or 12")));
}

RC_TEST_STEP(expression, comparisons, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("3=3"),   value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("3=4"),   value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("3==3"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("3!=4"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("3<>3"),  value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("2<3"),   value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("3<=3"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("4>=5"),  value_make_bool(false)));
    // arithmetic binds tighter than comparison, comparison tighter than and/or; two
    // comparison results are booleans, so the 'and' between them is the logical one.
    RC_CHECK_TRUE(value_is_equal(VAL("1+1=2"),         value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("2<3 and 4<5"),   value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("0 or 1"),        value_make_numeric(1.0)));   // two numbers: bitwise
    // A boolean coerces to 1 / 0 in any numeric context, comparisons included.
    RC_CHECK_TRUE(value_is_equal(VAL("1 = TRUE"),      value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("2 = TRUE"),      value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE > FALSE"),  value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("TRUE + 1"),      value_make_numeric(2.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("(2>1)*(3>2)*5"), value_make_numeric(5.0)));   // the mask idiom
}

RC_TEST_STEP(expression, more_functions, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("LO(258)"),   value_make_numeric(2.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("HI(258)"),   value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("SQRT(9)"),   value_make_numeric(3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("INT(2.7)"),  value_make_numeric(2.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("INT(-2.7)"), value_make_numeric(-3.0)));   // floor, toward -inf
    RC_CHECK_TRUE(value_is_equal(VAL("FLOOR(-2.7)"), value_make_numeric(-3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("ROUND(2.7)"),  value_make_numeric(3.0)));  // to nearest, halves away from zero
    RC_CHECK_TRUE(value_is_equal(VAL("ROUND(-2.7)"), value_make_numeric(-3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("ROUND(2.5)"),  value_make_numeric(3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("ROUND(-2.5)"), value_make_numeric(-3.0)));
    RC_CHECK_TRUE(value_is_equal(VAL("TRUNC(2.7)"),  value_make_numeric(2.0)));  // toward zero
    RC_CHECK_TRUE(value_is_equal(VAL("TRUNC(-2.7)"), value_make_numeric(-2.0)));
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

    // Empty axes are real lengths, not absences: {} is one axis of length 0, {{}} one row of nothing.
    value s0[] = {value_make_numeric(0)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape({})"), value_make_list((rc_view_value) RC_VIEW(s0))));
    value s10[] = {value_make_numeric(1), value_make_numeric(0)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape({{}})"), value_make_list((rc_view_value) RC_VIEW(s10))));

    // A range shapes as the rank-1 list it stands for, stepped and descending included; nested in a list
    // it contributes its length like any row. An unbounded range has no length to report (rank still 1).
    value sr10[] = {value_make_numeric(10)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape(0..9)"),     value_make_list((rc_view_value) RC_VIEW(sr10))));
    value sr5[] = {value_make_numeric(5)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape(0..2..9)"),  value_make_list((rc_view_value) RC_VIEW(sr5))));
    RC_CHECK_TRUE(value_is_equal(VAL("shape(9..5)"),     value_make_list((rc_view_value) RC_VIEW(sr5))));
    value s210[] = {value_make_numeric(2), value_make_numeric(10)};
    RC_CHECK_TRUE(value_is_equal(VAL("shape({0..9, 10..19})"), value_make_list((rc_view_value) RC_VIEW(s210))));
    RC_CHECK_TRUE(value_is_equal(VAL("rank(0..9)"), value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_equal(VAL("rank(2..)"),  value_make_numeric(1)));
    RC_CHECK_TRUE(value_is_error(VAL("shape(2..)")));
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

    value r4[] = {value_make_bool(false), value_make_bool(true), value_make_bool(false)};   // comparisons map
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

    // An exclusive range that does not ascend is a legal EMPTY list, not an error.
    RC_CHECK_TRUE(value_is_list(VAL("5..<5")) && VAL("5..<5").list.num == 0);
    RC_CHECK_TRUE(value_is_list(VAL("4..<1")) && VAL("4..<1").list.num == 0);   // descending exclusive
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
    RC_CHECK_TRUE(value_is_error(VAL("0..<2..6")));     // '<' on the wrong separator
    RC_CHECK_TRUE(value_is_error(VAL("(1..2)..3")));    // a range can't be a start
    RC_CHECK_TRUE(value_is_error(VAL("1..3..6..9")));   // inconsistent step (2 then 3)
    RC_CHECK_TRUE(value_is_error(VAL("0..3..4..10")));  // inconsistent step (1 then 3)
    RC_CHECK_TRUE(value_is_error(VAL("..2..6")));       // start-open and stepped
    RC_CHECK_TRUE(value_is_error(VAL("bar..9")));       // unknown symbol propagates

    // Enumerating a huge range (here by subscripting it) is capped at VALUE_LIST_MAX_LENGTH.
    RC_CHECK_TRUE(value_is_equal(VAL("(0..65535)[0]"), value_make_numeric(0)));   // exactly the cap is fine
    RC_CHECK_TRUE(VAL("(0..65536)[0]").error.code == error_type_list_too_big);        // one past it
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
    scopes_set_symbol(&fix->scopes, 0, RC_STR("foo"), value_make_numeric(42.0), (cursor){0, 0});
    RC_CHECK_TRUE(value_is_equal(VAL("defined(foo)"), value_make_bool(true)));    // resolves
    RC_CHECK_TRUE(value_is_equal(VAL("defined(bar)"), value_make_bool(false)));   // an unknown symbol

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

RC_TEST_STEP(expression, hex_operator, fix)
{
    // '~' formats a number as hex text: the narrowest of a byte / word / long that holds it, so the
    // length only ever steps at the width boundaries.
    RC_CHECK_TRUE(value_is_equal(VAL("~10"),      value_make_string(RC_STR("0A"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~65"),      value_make_string(RC_STR("41"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~&123"),    value_make_string(RC_STR("0123"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~65535"),   value_make_string(RC_STR("FFFF"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~0"),       value_make_string(RC_STR("00"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~255"),     value_make_string(RC_STR("FF"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~256"),     value_make_string(RC_STR("0100"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~&123456"), value_make_string(RC_STR("00123456"))));

    // The argument is read as a 32-bit pattern, exactly as NOT/AND do: negatives come out in two's
    // complement, a fraction truncates toward zero, and anything past the window wraps.
    RC_CHECK_TRUE(value_is_equal(VAL("~-1"),         value_make_string(RC_STR("FFFFFFFF"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~1.5"),        value_make_string(RC_STR("01"))));
    RC_CHECK_TRUE(value_is_equal(VAL("~4294967296"), value_make_string(RC_STR("00"))));

    // Same very low precedence as '<' and '>', so it swallows the whole following expression...
    RC_CHECK_TRUE(value_is_equal(VAL("~&1234+1"),   value_make_string(RC_STR("1235"))));
    RC_CHECK_TRUE(value_is_equal(VAL("len(~&1234)"), value_make_numeric(4)));

    // ...and, being a unary op, it broadcasts.
    value h[] = {value_make_string(RC_STR("01")), value_make_string(RC_STR("FF"))};
    RC_CHECK_TRUE(value_is_equal(VAL("~{1,255}"), value_make_list((rc_view_value) RC_VIEW(h))));

    // A string has no hex form.
    RC_CHECK_TRUE(value_is_error(VAL("~\"x\"")));
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
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" = \"abc\""), value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" = \"abd\""), value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" != \"abd\""), value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abc\" < \"abd\""), value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"abd\" > \"abc\""), value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("\"ab\" < \"abc\""),  value_make_bool(true)));   // prefix is less
    RC_CHECK_TRUE(value_is_error(VAL("\"a\" = 1")));                                  // mixed types

    // comparisons broadcast over a list of strings
    value cmp[] = {value_make_bool(true), value_make_bool(false)};
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

RC_TEST_STEP(expression, codes_and_chr, fix)
{
    // codes: a string's character codes as a rank-1 list; chr joins codes back into a string.
    value ab[] = {value_make_numeric(65), value_make_numeric(66)};
    RC_CHECK_TRUE(value_is_equal(VAL("CODES(\"AB\")"), value_make_list((rc_view_value) RC_VIEW(ab))));
    RC_CHECK_TRUE(value_is_equal(VAL("CODES(\"A\")[0]"), value_make_numeric(65)));   // the char-literal idiom

    value empty = VAL("CODES(\"\")");
    RC_CHECK_TRUE(value_is_list(empty));
    RC_CHECK(empty.list.num, ==, 0u);

    // The length-1 list broadcasts, so an offset remap needs no scalar at all.
    value off[] = {value_make_numeric(33), value_make_numeric(34)};
    RC_CHECK_TRUE(value_is_equal(VAL("CODES(\"AB\") - CODES(\" \")"), value_make_list((rc_view_value) RC_VIEW(off))));

    // codes is a broadcasting unary, so a list of strings maps element-wise.
    value cd[] = {value_make_numeric(67), value_make_numeric(68)};
    value pair[] = {value_make_list((rc_view_value) RC_VIEW(ab)), value_make_list((rc_view_value) RC_VIEW(cd))};
    RC_CHECK_TRUE(value_is_equal(VAL("CODES({\"AB\",\"CD\"})"), value_make_list((rc_view_value) RC_VIEW(pair))));

    RC_CHECK_TRUE(value_is_error(VAL("CODES(5)")));   // only strings have codes

    RC_CHECK_TRUE(value_is_equal(VAL("CHR(72)"),        value_make_string(RC_STR("H"))));
    RC_CHECK_TRUE(value_is_equal(VAL("CHR({72,73})"),   value_make_string(RC_STR("HI"))));   // structural join
    RC_CHECK_TRUE(value_is_equal(VAL("CHR(65..67)"),    value_make_string(RC_STR("ABC"))));  // ranges enumerate
    RC_CHECK_TRUE(value_is_equal(VAL("CHR({65,{66,67}})"), value_make_string(RC_STR("ABC"))));  // nested leaves flatten
    RC_CHECK_TRUE(value_is_equal(VAL("CHR(CODES(\"Hello!\"))"), value_make_string(RC_STR("Hello!"))));   // the round trip
    RC_CHECK_TRUE(value_is_equal(VAL("CHR(65.9)"),      value_make_string(RC_STR("A"))));    // fractions truncate

    RC_CHECK_TRUE(value_is_error(VAL("CHR(-1)")));     // no character to become
    RC_CHECK_TRUE(value_is_error(VAL("CHR(256)")));
    RC_CHECK_TRUE(value_is_error(VAL("CHR(\"x\")")));
}

RC_TEST_STEP(expression, find, fix)
{
    RC_CHECK_TRUE(value_is_equal(VAL("FIND({5,7,9}, 9)"),  value_make_numeric(2)));
    RC_CHECK_TRUE(value_is_equal(VAL("FIND({5,7,5}, 5)"),  value_make_numeric(0)));   // first occurrence
    RC_CHECK_TRUE(value_is_equal(VAL("FIND(10..20, 15)"),  value_make_numeric(5)));   // a range haystack enumerates

    // The needle broadcasts: a list of needles yields a same-shape list of indices - which is
    // exactly the character map, to[find(from, codes(s))].
    value ix[] = {value_make_numeric(2), value_make_numeric(0)};
    RC_CHECK_TRUE(value_is_equal(VAL("FIND({5,7,9}, {9,5})"), value_make_list((rc_view_value) RC_VIEW(ix))));
    value map[] = {value_make_numeric(2), value_make_numeric(0), value_make_numeric(1)};
    RC_CHECK_TRUE(value_is_equal(VAL("FIND(CODES(\"ABC\"), CODES(\"CAB\"))"), value_make_list((rc_view_value) RC_VIEW(map))));

    // A string haystack is a substring search.
    RC_CHECK_TRUE(value_is_equal(VAL("FIND(\"hello world\", \"world\")"), value_make_numeric(6)));
    RC_CHECK_TRUE(value_is_equal(VAL("FIND(\"hello\", \"\")"), value_make_numeric(0)));   // empty matches at the start

    // A miss is a loud not_found naming the needle, even from inside a broadcast.
    value miss = VAL("FIND({5,7,9}, 6)");
    RC_CHECK_TRUE(value_is_error(miss));
    RC_CHECK_TRUE(miss.error.code == error_type_not_found);
    RC_CHECK(miss.error.detail, ==, RC_STR("6"));
    value cmiss = VAL("FIND(CODES(\"ABC\"), CODES(\"AQ\"))");
    RC_CHECK_TRUE(value_is_error(cmiss));
    RC_CHECK_TRUE(cmiss.error.code == error_type_not_found);
    value smiss = VAL("FIND(\"hello\", \"z\")");
    RC_CHECK_TRUE(value_is_error(smiss));
    RC_CHECK_TRUE(smiss.error.code == error_type_not_found);
    RC_CHECK(smiss.error.detail, ==, RC_STR("z"));   // raw, not quoted

    RC_CHECK_TRUE(value_is_error(VAL("FIND(\"hello\", 5)")));   // a number is not a substring
    RC_CHECK_TRUE(value_is_error(VAL("FIND(5, 1)")));           // a scalar has nothing to search
    RC_CHECK_TRUE(value_is_error(VAL("FIND({1,2})")));          // arity
}

RC_TEST_STEP(expression, type_predicates, fix)
{
    // Whole-value predicates, deliberately not element-wise: a list is neither.
    RC_CHECK_TRUE(value_is_equal(VAL("IS_STRING(\"a\")"),   value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("IS_STRING(5)"),       value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("IS_STRING({\"a\"})"), value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("IS_NUMBER(5)"),       value_make_bool(true)));
    RC_CHECK_TRUE(value_is_equal(VAL("IS_NUMBER(TRUE)"),    value_make_bool(true)));   // a bool coerces to a number
    RC_CHECK_TRUE(value_is_equal(VAL("IS_NUMBER(\"a\")"),   value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("IS_NUMBER({1,2})"),   value_make_bool(false)));
    RC_CHECK_TRUE(value_is_equal(VAL("IS_NUMBER(1..3)"),    value_make_bool(false)));

    // Errors propagate (only defined() inspects), so a forward reference defers, not answers.
    value fwd = VAL("IS_NUMBER(nosuch)");
    RC_CHECK_TRUE(value_is_error(fwd));
    RC_CHECK_TRUE(fwd.error.code == error_type_unknown_symbol);
}

RC_TEST_STEP(expression, error_function, fix)
{
    // error(...) is the ERROR statement as a value: a user_error carrying the formatted message.
    value e = VAL("ERROR(\"oops \", 42)");
    RC_CHECK_TRUE(value_is_error(e));
    RC_CHECK_TRUE(e.error.code == error_type_user_error);
    RC_CHECK(e.error.detail, ==, RC_STR("oops 42"));

    // It screens its arguments first, so a forward reference defers instead of firing.
    value fwd = VAL("ERROR(\"width was \", nosuch)");
    RC_CHECK_TRUE(value_is_error(fwd));
    RC_CHECK_TRUE(fwd.error.code == error_type_unknown_symbol);

    // And like any error value it short-circuits through operators.
    value thru = VAL("1 + ERROR(\"bang\")");
    RC_CHECK_TRUE(value_is_error(thru));
    RC_CHECK_TRUE(thru.error.code == error_type_user_error);
}

#undef RESULT
#undef VAL

#endif // BARON_TESTS
