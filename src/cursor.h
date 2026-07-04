#ifndef BARON_CURSOR_H_
#define BARON_CURSOR_H_

#include <stdint.h>
#include <stdbool.h>
#include "richc/macros.h"   // RC_INDEX_NONE


// A position in a source: the source file index plus a byte offset into it. It is the parser's
// cursor, and doubles as the stable identity of a binding (the position that defined it) and of an
// anonymous scope. The source index is carried now even though it is always 0 until INCLUDE arrives.
typedef struct cursor {
    uint32_t source;
    uint32_t pos;
} cursor;

static inline bool cursor_is_equal(cursor a, cursor b)
{
    return a.source == b.source && a.pos == b.pos;
}

// The "no such position" cursor - RC_INDEX_NONE in both fields - returned where a cursor lookup can miss
// (a real source offset is never RC_INDEX_NONE), so we return it by value rather than via an out-param.
static inline cursor cursor_none(void)
{
    return (cursor) {.source = RC_INDEX_NONE, .pos = RC_INDEX_NONE};
}

static inline bool cursor_is_none(cursor c)
{
    return c.pos == RC_INDEX_NONE;
}

// The same source cursor with its offset moved to `pos` - the common "same source, a little further
// along" step, and the way a handler builds an error location from its own cursor plus an offset.
static inline cursor cursor_at(cursor at, uint32_t pos)
{
    at.pos = pos;
    return at;
}


#endif // ifndef BARON_CURSOR_H_
