#ifndef BARON_SOURCE_POS_H_
#define BARON_SOURCE_POS_H_

#include <stdint.h>
#include <stdbool.h>


// Where in the source a thing lives: a source file index (into source_files) plus a byte offset
// within it. We use it as a stable identity - the position that defined a symbol, or that opened a
// scope - so the same statement re-walked on a later pass lands on the same identity while a
// genuinely different statement does not. The source index is carried now even though it is always
// 0 until INCLUDE arrives, so multi-file assembly is a non-event later.
typedef struct source_pos {
    uint32_t source;
    uint32_t offset;
} source_pos;

static inline bool source_pos_is_equal(source_pos a, source_pos b)
{
    return a.source == b.source && a.offset == b.offset;
}


#endif // ifndef BARON_SOURCE_POS_H_
