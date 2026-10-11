#ifndef BARON_BASIC_H_
#define BARON_BASIC_H_

#include "error.h"
#include "richc/bytes.h"


// The BBC BASIC 4 line tokeniser. This is the ROM's own algorithm (the Master's BASIC IV tokeniser at
// &8DB2, keyword table at &8456) transplanted rather than reinvented, so the bytes are exactly what
// typing the line into a real machine would store: keywords become single tokens, line numbers after
// GOTO and friends become the &8D three-byte form, strings and REMs pass through untouched.
//
// A stored line is a record: 0D, the line number (high byte first), a length byte counting the whole
// record, itself included, then the tokenised text. The assembler builds the record around what we
// return, because a BASIC block can splice Baron's own bytes into a line ('{' ... '}'), cutting its
// text into pieces we only ever see one at a time.

enum { basic_max_record = 255 };   // the length is one byte, so no record can be longer

// Where the tokeniser stands between two pieces of one line. Handing each piece the state the last
// one left makes a splice invisible: a string or a REM carries straight on through it. Only a token
// cannot span one, so PRI{...}NT is two variable names, never PRINT.
typedef struct basic_state {
    bool sos;      // at a statement start: decides '*' commands and the pseudo-variable +&40 rule
    bool lnexp;    // a number here would be a line number (GOTO 10)
    bool quoted;   // inside a string, waiting for its closing '"'
    bool raw;      // REM, DATA or a '*' command owns the rest of the line
} basic_state;

// The state where a line's text begins, just past its number: at a statement start and - the ROM
// leaves it set - still expecting a line number, which is why even a bare 10 20 embeds one.
static inline basic_state basic_state_start(void)
{
    return (basic_state) {.sos = true, .lnexp = true};
}

typedef struct basic_number_result {
    uint32_t number;   // 0 on error
    uint32_t next;     // offset just past the digits: where the line's text begins
    uint16_t error;    // error_type
} basic_number_result;

// Read the decimal line number off the front of line (the caller peeked a digit - we assert). The ROM
// takes 0 to 32767; anything bigger is an error.
basic_number_result basic_line_number(rc_str line);

typedef struct basic_text_result {
    rc_view_bytes bytes;   // the tokenised text
    basic_state   state;   // where the tokeniser stands after it, for the next piece
} basic_text_result;

// Tokenise one piece of a line's text, starting from state. last marks the piece that ends the line,
// which gets the ROM's line-insert tidy: trailing spaces (and a stray '\r' from a CRLF source) are
// dropped. Every other space is kept exactly as written (LISTO 0, the power-on default).
basic_text_result basic_tokenise(rc_str text, basic_state state, bool last, rc_arena *arena);


#endif // ifndef BARON_BASIC_H_
