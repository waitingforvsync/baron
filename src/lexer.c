#include "lexer.h"

#include <float.h>


// ---- character predicates ----

static bool is_eof(rc_str text, uint32_t pos)  { return pos == text.len; }
static char at(rc_str text, uint32_t pos)      { RC_ASSERT(pos < text.len); return text.data[pos]; }

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

static uint32_t skip_comment(rc_str text, uint32_t pos)
{
    // Comments run to end of line (the newline is left for the terminator) or EOF.
    // Colon separators don't terminate comments
    RC_ASSERT(!is_eof(text, pos) && is_comment_start(at(text, pos)));

    while (!is_eof(text, pos) && !is_newline(at(text, pos))) {
        pos++;
    }

    return pos;
}

uint32_t lexer_skip_whitespace(rc_str text, uint32_t pos)
{
    // Skip whitespace and any trailing comments
    while (!is_eof(text, pos) && is_whitespace(at(text, pos))) {
        pos++;
    }

    if (!is_eof(text, pos) && is_comment_start(at(text, pos))) {
        pos = skip_comment(text, pos);
    }

    return pos;
}

// Coalesce a run of ':' / newline (and the whitespace/comments between them) into a
// single terminator lexeme. We enter at the first terminator character. The lexeme's
// newline flag is true only when the whole run was newlines; a ':' makes it the
// hard kind that a list will not skip over (as does EOF, handled separately).
static lexer_result lex_terminator(rc_str text, uint32_t pos)
{
    RC_ASSERT(!is_eof(text, pos) && is_terminator(at(text, pos)));

    bool only_newlines = true;
    do {
        if (at(text, pos) == ':') {
            only_newlines = false;
        }
        pos = lexer_skip_whitespace(text, pos + 1);
    }

    while (!is_eof(text, pos) && is_terminator(at(text, pos)));

    return make_result(
        (lexeme) {
            .type = lexeme_type_terminator,
            .terminator = { .newline = only_newlines }
        },
        pos
    );
}


// ---- number literals (hand-rolled; destined for richc) ----

static lexer_result lex_decimal(rc_str text, uint32_t pos)
{
    // We enter with pos at a digit.
    RC_ASSERT(!is_eof(text, pos) && is_digit(at(text, pos)));

    double result = 0.0;
    do {
        if (result > (DBL_MAX - (at(text, pos) - '0')) / 10.0) {
            return make_error(lexer_error_numeric_overflow, pos);
        }
        result = result * 10.0 + (at(text, pos) - '0');
        pos++;
    }

    while (!is_eof(text, pos) && is_digit(at(text, pos)));

    // Fractional part, only if there is a digit after the '.'
    // There's a subtlety here: we don't want to consume a '.' if it is not followed by a digit, so
    // that it can be parsed as a potential range operator (..)
    if (!is_eof(text, pos) && at(text, pos) == '.') {
        if (!is_eof(text, pos + 1) && is_digit(at(text, pos + 1))) {
            double divide = 1.0;
            bool underflow = false;
            pos++;  // consume '.'
            while (!is_eof(text, pos) && is_digit(at(text, pos))) {
                underflow |= (result > (DBL_MAX - (at(text, pos) - '0')) / 10.0);
                if (!underflow) {
                    result = result * 10.0 + (at(text, pos) - '0');
                    divide *= 10.0;
                }
                pos++;  // keep consuming digits even once precision is exhausted
            }
            result /= divide;
        }
    }

    return make_numeric(result, pos);
}


static lexer_result lex_hex(rc_str text, uint32_t pos)
{
    // We enter with pos at the first hex digit after the prefix
    if (is_eof(text, pos) || !is_hex_digit(at(text, pos))) {
        return make_error(lexer_error_bad_hex_literal, pos);
    }

    uint32_t start = pos;
    uint32_t result = 0;
    do {
        if (result > 0x0FFFFFFFu) {
            // @todo: don't abort immediately - flag the overflow and keep consuming digits so the pos is at the end of the token
            return make_error(lexer_error_numeric_overflow, start - 1);
        }
        result = (result << 4) | hex_value(at(text, pos));
        pos++;
    }

    while (!is_eof(text, pos) && is_hex_digit(at(text, pos)));

    return make_numeric((double)result, pos);
}

static lexer_result lex_binary(rc_str text, uint32_t pos)
{
    // We enter with pos at the first binary digit after the prefix
    if (is_eof(text, pos) || !is_bin_digit(at(text, pos))) {
        return make_error(lexer_error_bad_binary_literal, pos);
    }

    uint32_t start = pos;
    uint32_t result = 0;
    do {
        if (result > 0x7FFFFFFFu) {
            // @todo: don't abort immediately - flag the overflow and keep consuming digits so the pos is at the end of the token
            return make_error(lexer_error_numeric_overflow, start - 1);
        }
        result = (result << 1) | (uint32_t)(at(text, pos) - '0');
        pos++;
    }

    while (!is_eof(text, pos) && is_bin_digit(at(text, pos)));

    return make_numeric((double)result, pos);
}

static lexer_result lex_char(rc_str text, uint32_t pos)
{
    // pos is at quoted character. Expect X' - one char then a closing quote.
    if (is_eof(text, pos) || is_newline(at(text, pos)) || at(text, pos) == '\'' || is_eof(text, pos + 1) || at(text, pos + 1) != '\'') {
        return make_error(lexer_error_bad_char_literal, pos);
    }

    return make_numeric((double)(uint8_t)at(text, pos), pos + 2);
}


// ---- string literals ----

static lexer_result lex_string(rc_str text, uint32_t pos)
{
    // pos is past the opening quote. A doubled quote ("") is an escaped quote and
    // keeps the string going; we record that the raw text needs unescaping later.
    bool contains_quotes = false;

    uint32_t start = pos;
    while (true) {
        do {
            if (is_eof(text, pos) || is_newline(text.data[pos])) {
                return make_error(lexer_error_unterminated_string, pos - 1);
            }
        }
        while (text.data[pos++] != '\"');

        if (is_eof(text, pos) || text.data[pos] != '\"') {
            break;
        }

        contains_quotes = true;
        pos++;
    }

    return make_result(
        (lexeme) {
            .type = contains_quotes ? lexeme_type_escaped_string_literal : lexeme_type_string_literal,
            .string_literal = {
                .ref = rc_str_substr(text, start, pos - start - 1)
            }
        },
        pos
    );
}


// ---- identifiers (possibly dotted) ----

static uint32_t scan_segment(rc_str text, uint32_t pos)
{
    // pos is at an identifier-start char.
    RC_ASSERT(!is_eof(text, pos) && is_ident_start(at(text, pos)));

    do {
        pos++;
    }

    while (!is_eof(text, pos) && is_ident_char(at(text, pos)));

    return pos;
}

// Scan ident('.' ident)*, consuming a '.' only when an identifier-start follows
// (so a.b is one identifier, but a..b stops at a).
static uint32_t scan_dotted_identifier(rc_str text, uint32_t pos)
{
    pos = scan_segment(text, pos);
    while (!is_eof(text, pos) && at(text, pos) == '.'
           && !is_eof(text, pos + 1) && is_ident_start(at(text, pos + 1))) {
        pos = scan_segment(text, pos + 1);
    }

    return pos;
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

lexer_result lexer_next(rc_str text, uint32_t pos, token_table tt)
{
    RC_ASSERT(rc_str_is_valid(text));
    RC_ASSERT(rc_view_token_is_valid(tt));

    // First skip over whitespace and any trailing comment
    pos = lexer_skip_whitespace(text, pos);

    // If reached end of file, return a terminator and don't advance the pos
    if (is_eof(text, pos)) {
        return make_simple(lexeme_type_terminator, pos);
    }

    char c = at(text, pos);

    // If reached the end of the line, or a separator, return a separator lexeme
    if (is_terminator(c)) {
        return lex_terminator(text, pos);
    }

    // Parse string literals
    if (c == '"') {
        return lex_string(text, pos + 1);
    }

    // Parse a decimal number literal
    if (is_digit(c)) {
        return lex_decimal(text, pos);
    }

    // Parse a hex number literal
    if (is_hex_prefix(c)) {
        return lex_hex(text, pos + 1);
    }

    // Parse a binary number literal
    if (is_bin_prefix(c)) {
        return lex_binary(text, pos + 1);
    }

    // Parse a character literal
    if (c == '\'') {
        return lex_char(text, pos + 1);
    }

    // Parse a comma (commas are commas regardless of context)
    if (c == ',') {
        return make_simple(lexeme_type_comma, pos + 1);
    }

    // Operators / keywords from the context table. An identifier starting here
    // wins if it is longer than the matched token (so ANDY beats the AND token).
    uint32_t tok = token_table_find(tt, rc_str_skip(text, pos));
    if (tok != RC_INDEX_NONE) {
        rc_str name = rc_view_token_get(tt, tok).name;
        if (is_ident_start(c)) {
            uint32_t end = scan_dotted_identifier(text, pos);
            if (end - pos > name.len) {
                return make_identifier(text, pos, end);
            }
        }
        return make_result(rc_view_token_get(tt, tok).lexeme, pos + name.len);
    }

    // Identifier is the last resort.
    if (is_ident_start(c)) {
        return make_identifier(text, pos, scan_dotted_identifier(text, pos));
    }

    return make_error(lexer_error_unexpected_char, pos);
}


bool lexer_at_end(rc_str text, uint32_t pos)
{
    return is_eof(text, pos);
}

uint32_t lexer_line_end(rc_str text, uint32_t pos)
{
    while (!is_eof(text, pos) && !is_newline(at(text, pos))) {
        pos++;
    }

    return pos;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// A small context table for the lexer tests. Handlers are NULL - the lexer never
// invokes them.
static const token lexer_test_tokens[] = {
    { RC_STR_INIT("+"),   { .type = lexeme_type_binary_op } },
    { RC_STR_INIT(".."),  { .type = lexeme_type_binary_op } },
    { RC_STR_INIT("."),   { .type = lexeme_type_binary_op } },
    { RC_STR_INIT("and"), { .type = lexeme_type_binary_op } },
    { RC_STR_INIT("("),   { .type = lexeme_type_open_paren } },
    { RC_STR_INIT(")"),   { .type = lexeme_type_close_paren } },
    { RC_STR_INIT("{"),   { .type = lexeme_type_open_brace } },
    { RC_STR_INIT("}"),   { .type = lexeme_type_close_brace } },
    { RC_STR_INIT("["),   { .type = lexeme_type_open_bracket } },
    { RC_STR_INIT("]"),   { .type = lexeme_type_close_bracket } },
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

    // Greedy: ANDY is an identifier even though and is a token.
    r = lexer_next(RC_STR("ANDY"), 0, lexer_tt);
    RC_CHECK_TRUE(r.token.type == lexeme_type_identifier);
    RC_CHECK(r.token.identifier.name, ==, RC_STR("ANDY"));

    // and on its own is the token.
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

RC_TEST(lexer, line_end)
{
    // A raw scan to the next '\n': nothing else ends a line - not '\r', not ':',
    // not a comma - and a pos already on the '\n' stays put.
    rc_str s = RC_STR("10PRINT \"A:B\"\r\n20GOTO 10");
    RC_CHECK(lexer_line_end(s, 0),  ==, 14u);
    RC_CHECK(lexer_line_end(s, 14), ==, 14u);
    RC_CHECK(lexer_line_end(s, 15), ==, s.len);      // last line: no '\n' -> text.len
    RC_CHECK(lexer_line_end(RC_STR(""), 0), ==, 0u);
}

RC_TEST(lexer, skip_whitespace)
{
    // Blanks and a trailing comment are skipped; the comment's newline is left (it is terminator
    // territory), as are ':' and any actual token.
    RC_CHECK(lexer_skip_whitespace(RC_STR("  \t42"), 0), ==, 3u);
    RC_CHECK(lexer_skip_whitespace(RC_STR("  ; note\n42"), 0), ==, 8u);
    RC_CHECK(lexer_skip_whitespace(RC_STR(" :42"), 0), ==, 1u);
    RC_CHECK(lexer_skip_whitespace(RC_STR("42"), 0), ==, 0u);
    RC_CHECK(lexer_skip_whitespace(RC_STR("  "), 0), ==, 2u);
}

#endif // BARON_TESTS
