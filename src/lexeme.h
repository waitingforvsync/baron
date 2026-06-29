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
    lexeme_type_elif,                   // ELIF / ELSE / ENDIF: bare markers the IF chain reads, and
    lexeme_type_else,                   //   that close a statement block from the outside (like '}')
    lexeme_type_endif,
    lexeme_type_unary_op,
    lexeme_type_binary_op,
    lexeme_type_function,               // a bracketed call like ABS(x)
    lexeme_type_range,                  // '..' / '..<', the range operator
    lexeme_type_opcode,                 // a 6502 mnemonic (carries the mnemonic id)
    lexeme_type_keyword,                // a statement directive with a baked-in handler (ORG, '.')
    lexeme_type_hash,                   // '#', the immediate-operand marker
    lexeme_type_assign,                 // '=', symbol definition
    lexeme_type_register,               // A / X / Y, in opcode-operand context
    lexeme_type_error,
    // deferred until later milestones: constant
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
    // Variadic and structural: the handler gets the whole evaluated argument list and is
    // responsible for checking the count and types. Element-wise things (abs, sqrt, ...)
    // are unary ops, not functions, so they keep their simple scalar handler.
    value (*apply)(rc_view_value args, rc_arena *arena);
} lexeme_function;


typedef struct lexeme_range {
    bool exclusive;   // '..<' excludes the end value; '..' includes it
} lexeme_range;


// A 6502 mnemonic. id is a `mnemonic` enum value, kept as a uint16_t because lexeme.h must
// not depend on opcodes.h; the operand handler decodes it.
typedef struct lexeme_opcode {
    uint16_t id;
} lexeme_opcode;


// A 6502 register named in an operand: the accumulator A, or an index register X / Y. Only
// meaningful in opcode-operand context (its own token table), where A marks accumulator mode
// and X / Y mark the index.
typedef enum reg_name { reg_a, reg_x, reg_y } reg_name;
typedef struct lexeme_register {
    reg_name which;
} lexeme_register;


// A statement-level directive (ORG, the '.' label introducer, the '{' scope opener, and later
// INCLUDE / IF / FOR). The handler parses the rest of the statement itself and returns its
// outcome by value, so the dispatcher just calls it. Its assembler input/output and container
// types are only forward-declared here: a function-pointer declaration may use incomplete
// types by value, so this header stays free of any assembler include. Only the file that builds
// the token table and calls the handler (assemble.c) needs the complete types.
typedef struct baron baron;
typedef struct parse_result parse_result;
typedef struct parse_flags parse_flags;
typedef struct cursor cursor;
typedef struct lexeme_keyword {
    parse_result (*handle)(baron *b, cursor at, uint32_t scope,
                           parse_flags flags, rc_arena scratch);
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
        lexeme_opcode opcode;
        lexeme_register reg;
        lexeme_keyword keyword;
        lexeme_error error;
    };
} lexeme;



#endif // ifndef BARON_LEXEME_H_
