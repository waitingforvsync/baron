#include "lexer.h"

#include <float.h>


// ---- character predicates ----

static bool is_eof(rc_str text, uint32_t cursor)  { return cursor == text.len; }
static char at(rc_str text, uint32_t cursor)      { RC_ASSERT(cursor < text.len); return text.data[cursor]; }

static bool is_whitespace(char c)    { return c == ' ' || c == '\t' || c == '\r'; }
static bool is_newline(char c)       { return c == '\n'; }
static bool is_terminator(char c)    { return is_newline(c) || c == ':'; }
static bool is_comment_start(char c) { return c == ';' || c == '\\'; }
static bool is_digit(char c)         { return c >= '0' && c <= '9'; }
static bool is_hex_digit(char c)     { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_bin_digit(char c)     { return c == '0' || c == '1'; }
static bool is_hex_prefix(char c)    { return c == '&' || c == '$'; }
static bool is_bin_prefix(char c)    { return c == '%'; }
static bool is_ident_start(char c)   { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_ident_char(char c)    { return is_ident_start(c) || is_digit(c); }

static uint32_t hex_value(char c)
{
    if (c >= '0' && c <= '9') return (uint32_t) (c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t) (c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint32_t) (c - 'A' + 10);
    RC_UNREACHABLE();
}


// ---- result constructors ----

static lexer_result make_result(lexeme lx, uint32_t next)
{
    return (lexer_result) {
        .token = lx,
        .next = next
    };
}

static lexer_result make_simple(lexeme_type type, uint32_t next)
{
    return make_result(
        (lexeme) {
            .type = type
        },
        next
    );
}

static lexer_result make_numeric(double value, uint32_t next)
{
    return make_result(
        (lexeme) {
            .type = lexeme_type_numeric_literal,
            .numeric_literal = {
                .value = value
            }
        },
        next
    );
}

static lexer_result make_error(lexer_error error, uint32_t next)
{
    return make_result(
        (lexeme) {
            .type = lexeme_type_error,
            .error = {error}
        },
        next
    );
}


// ---- whitespace, comments, terminators ----

static uint32_t skip_comment(rc_str text, uint32_t cursor)
{
    // Comments run to end of line (the newline is left for the terminator) or EOF.
    // Colon separators don't terminate comments
    RC_ASSERT(!is_eof(text, cursor) && is_comment_start(at(text, cursor)));

    while (!is_eof(text, cursor) && !is_newline(at(text, cursor))) {
        cursor++;
    }
    return cursor;
}

static uint32_t skip_whitespace(rc_str text, uint32_t cursor)
{
    // Skip whitespace and any trailing comments
    while (!is_eof(text, cursor) && is_whitespace(at(text, cursor))) {
        cursor++;
    }

    if (!is_eof(text, cursor) && is_comment_start(at(text, cursor))) {
        cursor = skip_comment(text, cursor);
    }

    return cursor;
}

// Coalesce a run of ':' / newline (and the whitespace/comments between them) into a
// single terminator lexeme. We enter at the first terminator character. The lexeme's
// `newline` flag is true only when the whole run was newlines; a ':' makes it the
// hard kind that a list will not skip over (as does EOF, handled separately).
static lexer_result lex_terminator(rc_str text, uint32_t cursor)
{
    RC_ASSERT(!is_eof(text, cursor) && is_terminator(at(text, cursor)));

    bool only_newlines = true;
    do {
        if (at(text, cursor) == ':') {
            only_newlines = false;
        }
        cursor = skip_whitespace(text, cursor + 1);
    }
    while (!is_eof(text, cursor) && is_terminator(at(text, cursor)));

    return make_result(
        (lexeme) {
            .type = lexeme_type_terminator,
            .terminator = { .newline = only_newlines }
        },
        cursor
    );
}


// ---- number literals (hand-rolled; destined for richc) ----

static lexer_result lex_decimal(rc_str text, uint32_t cursor)
{
    // We enter with cursor at a digit.
    RC_ASSERT(!is_eof(text, cursor) && is_digit(at(text, cursor)));

    double result = 0.0;
    do {
        if (result > (DBL_MAX - (at(text, cursor) - '0')) / 10.0) {
            return make_error(lexer_error_numeric_overflow, cursor);
        }
        result = result * 10.0 + (at(text, cursor) - '0');
        cursor++;
    }
    while (!is_eof(text, cursor) && is_digit(at(text, cursor)));

    // Fractional part, only if there is a digit after the '.'
    // There's a subtlety here: we don't want to consume a '.' if it is not followed by a digit, so
    // that it can be parsed as a potential range operator (..)
    if (!is_eof(text, cursor) && at(text, cursor) == '.') {
        if (!is_eof(text, cursor + 1) && is_digit(at(text, cursor + 1))) {
            double divide = 1.0;
            bool underflow = false;
            cursor++;  // consume '.'
            while (!is_eof(text, cursor) && is_digit(at(text, cursor))) {
                underflow |= (result > (DBL_MAX - (at(text, cursor) - '0')) / 10.0);
                if (!underflow) {
                    result = result * 10.0 + (at(text, cursor) - '0');
                    divide *= 10.0;
                }
                cursor++;  // keep consuming digits even once precision is exhausted
            }
            result /= divide;
        }
    }

    return make_numeric(result, cursor);
}


static lexer_result lex_hex(rc_str text, uint32_t cursor)
{
    // We enter with cursor at the first hex digit after the prefix
    if (is_eof(text, cursor) || !is_hex_digit(at(text, cursor))) {
        return make_error(lexer_error_bad_hex_literal, cursor);
    }

    uint32_t start = cursor;
    uint32_t result = 0;
    do {
        if (result > 0x0FFFFFFFu) {
            // @todo: don't abort immediately - flag the overflow and keep consuming digits so the cursor is at the end of the token
            return make_error(lexer_error_numeric_overflow, start - 1);
        }
        result = (result << 4) | hex_value(at(text, cursor));
        cursor++;
    }
    while (!is_eof(text, cursor) && is_hex_digit(at(text, cursor)));

    return make_numeric((double)result, cursor);
}

static lexer_result lex_binary(rc_str text, uint32_t cursor)
{
    // We enter with cursor at the first binary digit after the prefix
    if (is_eof(text, cursor) || !is_bin_digit(at(text, cursor))) {
        return make_error(lexer_error_bad_binary_literal, cursor);
    }

    uint32_t start = cursor;
    uint32_t result = 0;
    do {
        if (result > 0x7FFFFFFFu) {
            // @todo: don't abort immediately - flag the overflow and keep consuming digits so the cursor is at the end of the token
            return make_error(lexer_error_numeric_overflow, start - 1);
        }
        result = (result << 1) | (uint32_t)(at(text, cursor) - '0');
        cursor++;
    }
    while (!is_eof(text, cursor) && is_bin_digit(at(text, cursor)));

    return make_numeric((double)result, cursor);
}

static lexer_result lex_char(rc_str text, uint32_t cursor)
{
    // cursor is at quoted character. Expect X' - one char then a closing quote.
    if (is_eof(text, cursor) || is_newline(at(text, cursor)) || at(text, cursor) == '\'' || is_eof(text, cursor + 1) || at(text, cursor + 1) != '\'') {
        return make_error(lexer_error_bad_char_literal, cursor);
    }
    return make_numeric((double)(uint8_t)at(text, cursor), cursor + 2);
}


// ---- string literals ----

static lexer_result lex_string(rc_str text, uint32_t cursor)
{
    // cursor is past the operning quote. A doubled quote ("") is an escaped quote and
    // keeps the string going; we record that the raw text needs unescaping later.
    bool contains_quotes = false;

    uint32_t start = cursor;
    while (true) {
        do {
            if (is_eof(text, cursor) || is_newline(text.data[cursor])) {
                return make_error(lexer_error_unterminated_string, cursor - 1);
            }
        }
        while (text.data[cursor++] != '\"');

        if (is_eof(text, cursor) || text.data[cursor] != '\"') {
            break;
        }

        contains_quotes = true;
        cursor++;
    }

    return make_result(
        (lexeme) {
            .type = contains_quotes ? lexeme_type_escaped_string_literal : lexeme_type_string_literal,
            .string_literal = {
                .ref = rc_str_substr(text, start, cursor - start - 1)
            }
        },
        cursor
    );
}


// ---- identifiers (possibly dotted) ----

static uint32_t scan_segment(rc_str text, uint32_t cursor)
{
    // cursor is at an identifier-start char.
    RC_ASSERT(!is_eof(text, cursor) && is_ident_start(at(text, cursor)));

    do {
        cursor++;
    }
    while (!is_eof(text, cursor) && is_ident_char(at(text, cursor)));

    return cursor;
}

// Scan ident('.' ident)*, consuming a '.' only when an identifier-start follows
// (so `a.b` is one identifier, but `a..b` stops at `a`).
static uint32_t scan_dotted_identifier(rc_str text, uint32_t cursor)
{
    cursor = scan_segment(text, cursor);
    while (!is_eof(text, cursor) && at(text, cursor) == '.'
           && !is_eof(text, cursor + 1) && is_ident_start(at(text, cursor + 1))) {
        cursor = scan_segment(text, cursor + 1);
    }
    return cursor;
}

static lexer_result make_identifier(rc_str text, uint32_t start, uint32_t end)
{
    return make_result(
        (lexeme) {
            .type = lexeme_type_identifier,
            .identifier = {
                .name = rc_str_substr(text, start, end - start)
            }
        },
        end
    );
}


// ---- main ----

lexer_result lexer_next(rc_str text, uint32_t cursor, token_table tt)
{
    RC_ASSERT(rc_str_is_valid(text));
    RC_ASSERT(rc_view_token_is_valid(tt));

    // First skip over whitespace and any trailing comment
    cursor = skip_whitespace(text, cursor);

    // If reached end of file, return a terminator and don't advance the cursor
    if (is_eof(text, cursor)) {
        return make_simple(lexeme_type_terminator, cursor);
    }

    char c = at(text, cursor);

    // If reached the end of the line, or a separator, return a separator lexeme
    if (is_terminator(c)) {
        return lex_terminator(text, cursor);
    }

    // Parse string literals
    if (c == '"') {
        return lex_string(text, cursor + 1);
    }

    // Parse a decimal number literal
    if (is_digit(c)) {
        return lex_decimal(text, cursor);
    }

    // Parse a hex number literal
    if (is_hex_prefix(c)) {
        return lex_hex(text, cursor + 1);
    }

    // Parse a binary number literal
    if (is_bin_prefix(c)) {
        return lex_binary(text, cursor + 1);
    }

    // Parse a character literal
    if (c == '\'') {
        return lex_char(text, cursor + 1);
    }

    // Parse a comma (commas are commas regardless of context)
    if (c == ',') {
        return make_simple(lexeme_type_comma, cursor + 1);
    }

    // Operators / keywords from the context table. An identifier starting here
    // wins if it is longer than the matched token (so ANDY beats the AND token).
    uint32_t tok = token_table_find(tt, rc_str_skip(text, cursor));
    if (tok != RC_INDEX_NONE) {
        rc_str name = rc_view_token_get(tt, tok).name;
        if (is_ident_start(c)) {
            uint32_t end = scan_dotted_identifier(text, cursor);
            if (end - cursor > name.len) {
                return make_identifier(text, cursor, end);
            }
        }
        return make_result(rc_view_token_get(tt, tok).lexeme, cursor + name.len);
    }

    // Identifier is the last resort.
    if (is_ident_start(c)) {
        return make_identifier(text, cursor, scan_dotted_identifier(text, cursor));
    }

    return make_error(lexer_error_unexpected_char, cursor);
}


bool lexer_at_end(rc_str text, uint32_t cursor)
{
    return is_eof(text, cursor);
}


#ifdef BARON_TESTS

#include "richc/test.h"

// A small context table for the lexer tests. Handlers are NULL - the lexer never
// invokes them.
static const token lexer_test_tokens[] = {
    { RC_STR("+"),   { .type = lexeme_type_binary_op } },
    { RC_STR(".."),  { .type = lexeme_type_binary_op } },
    { RC_STR("."),   { .type = lexeme_type_binary_op } },
    { RC_STR("and"), { .type = lexeme_type_binary_op } },
    { RC_STR("("),   { .type = lexeme_type_open_paren } },
    { RC_STR(")"),   { .type = lexeme_type_close_paren } },
    { RC_STR("{"),   { .type = lexeme_type_open_brace } },
    { RC_STR("}"),   { .type = lexeme_type_close_brace } },
    { RC_STR("["),   { .type = lexeme_type_open_bracket } },
    { RC_STR("]"),   { .type = lexeme_type_close_bracket } },
};

static const token_table lexer_tt = RC_VIEW(lexer_test_tokens);

RC_TEST(lexer, numbers)
{
    rc_str s = RC_STR("42 3.5 &FF $1A %1010 'A'");
    uint32_t p = 0;
    lexer_result r;

    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_numeric_literal); RC_CHECK(r.token.numeric_literal.value, ==, 42.0);  p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 3.5);   p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 255.0); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 26.0);  p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 10.0);  p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 65.0);  p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);
}

RC_TEST(lexer, number_errors)
{
    lexer_result r;
    r = lexer_next(RC_STR("&xy"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_error);
    RC_CHECK_TRUE(r.token.error.type == lexer_error_bad_hex_literal);

    r = lexer_next(RC_STR("%2"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_error);
    RC_CHECK_TRUE(r.token.error.type == lexer_error_bad_binary_literal);

    r = lexer_next(RC_STR("#"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_error);
    RC_CHECK_TRUE(r.token.error.type == lexer_error_unexpected_char);
}

RC_TEST(lexer, strings)
{
    lexer_result r;
    r = lexer_next(RC_STR("\"hello\""), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_string_literal);
    RC_CHECK(r.token.string_literal.ref, ==, RC_STR("hello"));

    // "a""b" -> escaped, raw text keeps the doubled quote
    r = lexer_next(RC_STR("\"a\"\"b\""), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_escaped_string_literal);
    RC_CHECK(r.token.string_literal.ref, ==, RC_STR("a\"\"b"));

    r = lexer_next(RC_STR("\"oops"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_error);
    RC_CHECK_TRUE(r.token.error.type == lexer_error_unterminated_string);
}

RC_TEST(lexer, identifiers_and_dots)
{
    lexer_result r;

    // Qualified path is one identifier.
    r = lexer_next(RC_STR("routine.core"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_identifier);
    RC_CHECK(r.token.identifier.name, ==, RC_STR("routine.core"));

    // a..b -> identifier "a", range "..", identifier "b".
    rc_str s = RC_STR("a..b");
    uint32_t p = 0;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_identifier); RC_CHECK(r.token.identifier.name, ==, RC_STR("a")); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_binary_op);  p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_identifier); RC_CHECK(r.token.identifier.name, ==, RC_STR("b"));

    // Greedy: ANDY is an identifier even though `and` is a token.
    r = lexer_next(RC_STR("ANDY"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_identifier);
    RC_CHECK(r.token.identifier.name, ==, RC_STR("ANDY"));

    // `and` on its own is the token.
    r = lexer_next(RC_STR("and "), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_binary_op);
}

RC_TEST(lexer, terminators_and_punctuation)
{
    // Consecutive ':' / newlines coalesce into one terminator.
    rc_str s = RC_STR("a:\n\n:b");
    uint32_t p = 0;
    lexer_result r;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_identifier);  RC_CHECK(r.token.identifier.name, ==, RC_STR("a")); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);  p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_identifier);  RC_CHECK(r.token.identifier.name, ==, RC_STR("b")); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);

    // EOF terminator does not advance; calling again is idempotent.
    uint32_t end = r.next;
    RC_CHECK(end, ==, s.len);
    lexer_result again = lexer_next(s, end, lexer_tt);
    RC_CHECK_TRUE(again.token.type == lexeme_type_terminator);
    RC_CHECK(again.next, ==, end);

    rc_str s2 = RC_STR("([,])");
    p = 0;
    r = lexer_next(s2, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_open_paren);    p = r.next;
    r = lexer_next(s2, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_open_bracket);  p = r.next;
    r = lexer_next(s2, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_comma);         p = r.next;
    r = lexer_next(s2, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_close_bracket); p = r.next;
    r = lexer_next(s2, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_close_paren);

    // Braces come from the token table (their own lexeme types).
    rc_str s3 = RC_STR("{}");
    p = 0;
    r = lexer_next(s3, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_open_brace);  p = r.next;
    r = lexer_next(s3, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_close_brace);

    // The newline flag distinguishes a soft (newline-only) terminator from a hard one.
    r = lexer_next(RC_STR("a\nb"), 1, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);
    RC_CHECK_TRUE(r.token.terminator.newline);             // pure newline -> soft
    r = lexer_next(RC_STR("a:b"), 1, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);
    RC_CHECK_FALSE(r.token.terminator.newline);            // a ':' -> hard
    r = lexer_next(RC_STR("a"), 1, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);
    RC_CHECK_FALSE(r.token.terminator.newline);            // EOF -> hard
}

RC_TEST(lexer, whitespace_and_comments)
{
    // Whitespace then a comment then a newline -> a terminator.
    lexer_result r = lexer_next(RC_STR("   ; comment\n  42"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);

    // A trailing comment is skipped after a token.
    rc_str s = RC_STR("  1 + 2 ; trailing");
    uint32_t p = 0;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 1.0); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_binary_op); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK(r.token.numeric_literal.value, ==, 2.0); p = r.next;
    r = lexer_next(s, p, lexer_tt); RC_CHECK_TRUE(r.token.type == lexeme_type_terminator);
}

#endif // BARON_TESTS
