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

// The offset of the next '\n' at or after pos, or text.len when none remains. A pure
// scan - nothing is consumed and no lexeme is formed: the caller slices the raw line
// (BASIC blocks hand it to the tokeniser whole) and lexes the terminator itself.
uint32_t lexer_line_end(rc_str text, uint32_t pos);

// The offset past any blanks and a trailing comment at pos - the same skip lexer_next
// performs before every token, so a caller peeking at raw text (BASIC's is-this-a-
// numbered-line check) shares the lexer's definition of "blank". Newlines and ':' are
// terminator LEXEMES, not blanks, so the skip stops at them (a comment ends at its
// newline, which is left in place).
uint32_t lexer_skip_whitespace(rc_str text, uint32_t pos);


#endif // ifndef BARON_LEXER_H_
