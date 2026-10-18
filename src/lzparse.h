#ifndef BARON_LZPARSE_H_
#define BARON_LZPARSE_H_

#include <stdbool.h>
#include <stdint.h>

#include "richc/array/i32.h"
#include "richc/bytes.h"


// The optimal parse behind LZ_PARSE: the cheapest way, in bits, to write data as runs that alternate
// literals, matches, literals, ... (always literals first), each run's count written before its
// contents. A match copies `length` bytes from `offset` bytes back (overlapping copies allowed).
//
// Nothing about how a field is written is known here: every cost comes from a table indexed by the
// value being written. A negative entry, or an index past the end of a table, means that value cannot
// be written at all. Three assumptions keep the search exact and fast: a match is at least 2 bytes long,
// an offset never gets cheaper as offsets grow, and a match run's count never gets cheaper as it grows
// (true of gamma codes and of bucketed offsets; an impossible count counts as dearest of all).

typedef struct lz_costs {
    rc_view_i32 literal_run;   // a literal run's header, by its count
    rc_view_i32 literal;       // one literal, by its byte value
    rc_view_i32 match_run;     // a match run's header, by its count
    rc_view_i32 length;        // a match's length, by length
    rc_view_i32 offset;        // a match's offset, by offset
} lz_costs;

typedef struct lz_match {
    uint32_t offset;
    uint32_t length;
} lz_match;

#define RC_ARRAY_TYPE lz_match
#define RC_ARRAY_NAME lz_match
#include "richc/template/array.h"

typedef struct lz_run {
    bool     is_match;
    uint32_t start;   // a literal run: its first byte in data; a match run: its first entry in matches
    uint32_t count;   // bytes, or matches
} lz_run;

#define RC_ARRAY_TYPE lz_run
#define RC_ARRAY_NAME lz_run
#include "richc/template/array.h"

typedef struct lz_parse_result {
    bool             ok;        // false: no parse exists under these tables
    int64_t          bits;      // the cheapest total, every run's header and contents included
    rc_view_lz_run   runs;      // in stream order
    rc_view_lz_match matches;   // every match run's matches, in order; runs index into them
} lz_parse_result;

// Everything - the search's working memory and the result - is allocated in arena, so a caller that
// wants the working memory back evaluates in a region it can drop (as a FUNCTION call does).
lz_parse_result lz_parse(rc_view_bytes data, lz_costs costs, rc_arena *arena);


#endif // ifndef BARON_LZPARSE_H_
