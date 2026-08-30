#ifndef BARON_TOKEN_H_
#define BARON_TOKEN_H_

#include "lexeme.h"


// A token maps a piece of source text to the lexeme it produces. Token tables
// are context-specific: the same text (e.g. "-") can be a unary or binary
// operator depending on which table the lexer is given.
typedef struct token {
    rc_str name;
    lexeme lexeme;
} token;

#define RC_ARRAY_TYPE token
#define RC_ARRAY_NAME token
#include "richc/template/array.h"

typedef rc_view_token token_table;

// Return the index of the entry whose name is the longest prefix of text
// (case-insensitive, since keywords/operators are case-insensitive), or
// RC_INDEX_NONE if none match.
uint32_t token_table_find(token_table tt, rc_str text);


#endif // ifndef BARON_TOKEN_H_
