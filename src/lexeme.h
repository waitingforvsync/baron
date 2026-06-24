#ifndef BARON_LEXEME_H_
#define BARON_LEXEME_H_

#include "value.h"   // value, and rc_str / rc_arena via value.h


// A lexeme is one token emitted by the lexer. It is a tagged union: the lexer
// produces literals/identifiers/terminators directly, while operators and
// keywords come verbatim from a context token table (see token.h). Lexer errors
// are carried as a lexeme rather than a separate channel, so the parser can keep
// going past a malformed token.

typedef enum lexer_error {
    lexer_error_none,                   // 0 / default
    lexer_error_unexpected_char,
    lexer_error_invalid_number,
    lexer_error_numeric_overflow,
    lexer_error_bad_hex_literal,
    lexer_error_bad_binary_literal,
    lexer_error_bad_char_literal,
    lexer_error_unterminated_string,
} lexer_error;


typedef enum lexeme_type {
    lexeme_type_none,                   // 0 / default (uninitialised)
    lexeme_type_terminator,             // end of statement: ':' / newline / EOF
    lexeme_type_numeric_literal,
    lexeme_type_string_literal,
    lexeme_type_escaped_string_literal, // raw text still contains doubled quotes
    lexeme_type_identifier,             // possibly dotted: routine.core
    lexeme_type_comma,
    lexeme_type_open_paren,
    lexeme_type_close_paren,
    lexeme_type_open_bracket,
    lexeme_type_close_bracket,
    lexeme_type_open_brace,
    lexeme_type_close_brace,
    lexeme_type_unary_op,
    lexeme_type_binary_op,
    lexeme_type_function,               // a bracketed call like ABS(x)
    lexeme_type_range,                  // '..' / '..<', the range operator
    lexeme_type_error,
    // deferred until later milestones: keyword, constant
} lexeme_type;


// How a binary operator groups when chained at equal precedence: a-b-c is
// (a-b)-c (left), a^b^c is a^(b^c) (right).
typedef enum assoc {
    assoc_left,                         // 0 / default
    assoc_right,
} assoc;


typedef struct lexeme_terminator {
    bool newline;   // true if the run was only newline(s): the soft kind a list ignores
} lexeme_terminator;


typedef struct lexeme_numeric_literal {
    double value;
} lexeme_numeric_literal;


typedef struct lexeme_string_literal {
    rc_str ref;
} lexeme_string_literal;


typedef struct lexeme_identifier {
    rc_str name;
} lexeme_identifier;


typedef struct lexeme_constant {
    // The handler will be a function pointer to the constant's evaluation function.
} lexeme_constant;


typedef struct lexeme_unary_op {
    value (*apply)(value v, rc_arena *arena);
    uint8_t precedence;                 // its operand is parsed at this precedence,
                                        // so -2^2 is -(2^2) and a future LO/HI can
                                        // swallow the whole expression that follows
} lexeme_unary_op;


typedef struct lexeme_binary_op {
    value (*apply)(value a, value b, rc_arena *arena);
    uint8_t precedence;                 // higher binds tighter
    assoc   associativity;
} lexeme_binary_op;


typedef struct lexeme_function {
    value (*apply)(value v, rc_arena *arena);   // one argument for now
    bool  aggregate;   // false (default): map element-wise over a list/range;
                       // true: take the whole value (e.g. shape)
} lexeme_function;


typedef struct lexeme_range {
    bool exclusive;   // '..<' excludes the end value; '..' includes it
} lexeme_range;


typedef struct lexeme_keyword {
    // The handler will be a function pointer to the keyword's handler
} lexeme_keyword;


typedef struct lexeme_error {
    uint32_t type;
} lexeme_error;


typedef struct lexeme {
    uint32_t type;
    union {
        lexeme_terminator terminator;
        lexeme_numeric_literal numeric_literal;
        lexeme_string_literal string_literal;
        lexeme_identifier identifier;
        lexeme_constant constant;
        lexeme_unary_op unary_op;
        lexeme_binary_op binary_op;
        lexeme_function function;
        lexeme_range range;
        lexeme_keyword keyword;
        lexeme_error error;
    };
} lexeme;



#endif // ifndef BARON_LEXEME_H_
