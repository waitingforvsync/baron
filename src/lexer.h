#ifndef BARON_LEXER_H_
#define BARON_LEXER_H_

#include "lexeme.h"
#include "token.h"


typedef struct lexer_result {
    lexeme   token;
    uint32_t next;          // offset of the next character to lex from
} lexer_result;


// Lex one lexeme from text starting at offset pos, consulting tt for the
// context-specific operators and keywords. Stateless: returns the lexeme and the
// offset to continue from. At end of input it returns a terminator without
// advancing, so repeated calls at the end stay put.
lexer_result lexer_next(rc_str text, uint32_t pos, token_table tt);

// True when pos is at the end of text.
bool lexer_at_end(rc_str text, uint32_t pos);


#endif // ifndef BARON_LEXER_H_
