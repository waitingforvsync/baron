#ifndef BARON_SYMBOLS_H_
#define BARON_SYMBOLS_H_

#include "value.h"   // value (and rc_str, via richc/str.h)


// One resolved symbol, as it appears in an assemble's flattened symbol table: its full dotted path from
// the top level ("routine.core") and its value. Both are position-independent - the path bytes and the
// value's backing live in the arena the harvest wrote into - so an array of these is a plain read-only
// snapshot, safe to hold once the scope tree it came from is gone. scopes_view_flatten builds it on demand
// from a baron_result's `scopes` view.
typedef struct symbol_entry {
    rc_str path;
    value  v;
} symbol_entry;

#define RC_ARRAY_TYPE symbol_entry
#define RC_ARRAY_NAME symbol_entry
#include "richc/template/array.h"   // rc_{view,span,array}_symbol_entry


#endif // ifndef BARON_SYMBOLS_H_
