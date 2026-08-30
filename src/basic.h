#ifndef BARON_BASIC_H_
#define BARON_BASIC_H_

#include "error.h"
#include "richc/bytes.h"


// The BBC BASIC 4 line tokeniser: one line of BASIC source in, one complete tokenised line record out.
// This is the ROM's own algorithm (the Master's BASIC IV tokeniser at &8DB2, keyword table at &8456)
// transplanted rather than reinvented, so the bytes are exactly what typing the line into a real
// machine would store: keywords become single tokens, line numbers after GOTO and friends become the
// &8D three-byte form, strings and REMs pass through untouched. The one difference is mechanical - the
// ROM tokenises in place in the input buffer, whereas we build the record fresh in the caller's arena.

typedef struct basic_line_result {
    rc_view_bytes bytes;   // the whole record: 0D, line hi, line lo, length, tokenised text; empty on error
    uint16_t      error;   // error_type
} basic_line_result;

// Tokenise one whole source line. line starts at the first digit of the line number (the caller
// peeked it - we assert) and ends before the newline. The decimal line number becomes the record's
// big-endian header (the ROM's enterable range, 0 to 32767); spaces after it are kept exactly as
// written (LISTO 0, the power-on default) while trailing spaces - and a stray '\r' from a CRLF
// source - are stripped, as the ROM's line-insert routine always does. The length byte counts the
// whole record, 0D included, so a record past 255 bytes cannot be represented and errors instead.
basic_line_result basic_tokenise_line(rc_str line, rc_arena *arena);


#endif // ifndef BARON_BASIC_H_
