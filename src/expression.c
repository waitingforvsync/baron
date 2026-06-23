#include "expression.h"

#include "lexer.h"
#include "richc/macros.h"
#include <math.h>


// ---- precedence ladder (higher binds tighter) ----
// Unary +/- sit below pow so -2^2 is -(2^2); leaving headroom underneath for the
// LO/HI operators (< and >) to come, which want to swallow the whole expression.
enum {
    PREC_ADD = 10,   // + -
    PREC_MUL = 20,   // * /
    PREC_NEG = 30,   // unary + -
    PREC_POW = 40,   // ^  (right-associative)
};


// ---- operator and function handlers ----
// The apply_* wrappers below screen operands for error values first, so a handler
// only ever sees real values. The arena is unused by the numeric operators but
// stays in the signature for the string/list operators still to come.

static value op_add(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        return value_make_error(value_error_type_mismatch);
    }
    return value_make_numeric(a.numeric + b.numeric);
}

static value op_sub(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        return value_make_error(value_error_type_mismatch);
    }
    return value_make_numeric(a.numeric - b.numeric);
}

static value op_mul(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        return value_make_error(value_error_type_mismatch);
    }
    return value_make_numeric(a.numeric * b.numeric);
}

static value op_div(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        return value_make_error(value_error_type_mismatch);
    }
    if (b.numeric == 0.0) {
        return value_make_error(value_error_divide_by_zero);
    }
    return value_make_numeric(a.numeric / b.numeric);
}

static value op_pow(value a, value b, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        return value_make_error(value_error_type_mismatch);
    }
    double r = pow(a.numeric, b.numeric);
    if (isnan(r)) {
        return value_make_error(value_error_domain);   // e.g. a negative base, fractional exponent
    }
    return value_make_numeric(r);
}

static value op_neg(value v, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(v)) {
        return value_make_error(value_error_type_mismatch);
    }
    return value_make_numeric(-v.numeric);
}

static value op_pos(value v, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(v)) {
        return value_make_error(value_error_type_mismatch);
    }
    return v;
}

static value fn_abs(value v, rc_arena *arena)
{
    (void)arena;
    if (!value_is_numeric(v)) {
        return value_make_error(value_error_type_mismatch);
    }
    return value_make_numeric(fabs(v.numeric));
}


// ---- the two context tables ----
// EVEN: lexed where an operand is expected (the start, after a binary op, after an
// open paren). Numbers/strings/identifiers come from the lexer itself, so the table
// only carries the leading operators, the functions, and the open paren.
static const token even_tokens_[] = {
    { RC_STR("("),   { .type = lexeme_type_open_paren } },
    { RC_STR("+"),   { .type = lexeme_type_unary_op, .unary_op = { op_pos, PREC_NEG } } },
    { RC_STR("-"),   { .type = lexeme_type_unary_op, .unary_op = { op_neg, PREC_NEG } } },
    { RC_STR("ABS"), { .type = lexeme_type_function, .function = { fn_abs } } },
};

// ODD: lexed where a binary operator is expected (after an operand). The close paren
// lives here, so a parenthesised group is closed from operator position.
static const token odd_tokens_[] = {
    { RC_STR(")"),   { .type = lexeme_type_close_paren } },
    { RC_STR("+"),   { .type = lexeme_type_binary_op, .binary_op = { op_add, PREC_ADD, assoc_left } } },
    { RC_STR("-"),   { .type = lexeme_type_binary_op, .binary_op = { op_sub, PREC_ADD, assoc_left } } },
    { RC_STR("*"),   { .type = lexeme_type_binary_op, .binary_op = { op_mul, PREC_MUL, assoc_left } } },
    { RC_STR("/"),   { .type = lexeme_type_binary_op, .binary_op = { op_div, PREC_MUL, assoc_left } } },
    { RC_STR("^"),   { .type = lexeme_type_binary_op, .binary_op = { op_pow, PREC_POW, assoc_right } } },
};

static const token_table even_tokens = RC_VIEW(even_tokens_);
static const token_table odd_tokens  = RC_VIEW(odd_tokens_);


// ---- the parser ----
// The invariants for one parse, bundled so the recursive helpers stay readable.
typedef struct parser {
    rc_str        text;
    const scopes *scopes;
    uint32_t      scope_index;
    rc_arena     *arena;
} parser;

static value apply_binary(lexeme_binary_op op, value a, value b, rc_arena *arena)
{
    if (value_is_error(a)) return a;
    if (value_is_error(b)) return b;
    return op.apply(a, b, arena);
}

static value apply_unary(lexeme_unary_op op, value v, rc_arena *arena)
{
    return value_is_error(v) ? v : op.apply(v, arena);
}

static value apply_function(lexeme_function fn, value v, rc_arena *arena)
{
    return value_is_error(v) ? v : fn.apply(v, arena);
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

static expr_result parse_precedence(const parser *p, uint32_t cursor, uint8_t min_prec);

// Parse one operand: a literal, a symbol, a parenthesised group, a prefixed unary
// expression, or a function call. A lexeme that cannot begin an operand is a soft
// expected_expression failure, leaving the caller to decide whether that is fatal.
static expr_result parse_operand(const parser *p, uint32_t cursor)
{
    lexer_result lr = lexer_next(p->text, cursor, even_tokens);
    lexeme lex = lr.token;

    switch (lex.type) {
        case lexeme_type_numeric_literal:
            return ok(value_make_numeric(lex.numeric_literal.value), lr.next);

        case lexeme_type_string_literal:
        case lexeme_type_escaped_string_literal:
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

        default:
            // A terminator, a stray close paren, a lexer error: no operand here.
            return fail(expr_error_expected_expression, cursor);
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
        if (lr.token.type != lexeme_type_binary_op) {
            return lhs;   // not an operator: stop here, leaving the lexeme unconsumed
        }

        lexeme_binary_op op = lr.token.binary_op;
        if (op.precedence < min_prec) {
            return lhs;   // binds looser than the caller allows: leave it to them
        }

        // Left-assoc parses its right side one notch higher so a-b-c folds as (a-b)-c;
        // right-assoc keeps the level so a^b^c folds as a^(b^c).
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

#undef RESULT
#undef VAL

#endif // BARON_TESTS
