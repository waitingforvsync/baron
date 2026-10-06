#ifndef BARON_SYMBOLS_H_
#define BARON_SYMBOLS_H_

#include "value.h"
#include "cursor.h"


// What made a binding, in the order the symbol dump lists its groups. The first is the default.
typedef enum symbol_kind {
    symbol_kind_label,        // .name or .@ - always an address
    symbol_kind_assignment,   // name = expr, and a FUNCTION body's locals
    symbol_kind_define,       // -D on the command line
    symbol_kind_za_auto,      // a zero-page allocation
    symbol_kind_loop_var,     // a FOR variable, one per iteration
    symbol_kind_param,        // a macro or FUNCTION parameter
} symbol_kind;

// A bound symbol: its current value plus the source position that defined it. The position is the
// binding's identity - the same statement re-walked on a later pass carries the same def (so we
// re-evaluate and watch for a moved value), while a different statement defining the same name in
// the same scope is a duplicate. Baron's source is immutable, so a name is bound exactly once.
// The kind and section describe the binding without being part of that identity.
typedef struct symbol {
    value    v;
    cursor   def;
    uint32_t section;   // innermost section open at the defining statement
    uint8_t  kind;      // symbol_kind
} symbol;

// One resolved symbol in an assemble's flattened symbol table: its full dotted path from the top level
// ("routine.core") and the binding itself. Both are position-independent (their backing lives in the
// arena the harvest wrote into), so an array of these is a plain read-only snapshot that outlives the
// scope tree it came from. scopes_view_flatten builds it on demand from a baron_result's scopes view.
typedef struct symbol_entry {
    rc_str path;
    symbol sym;
} symbol_entry;

#define RC_ARRAY_TYPE symbol_entry
#define RC_ARRAY_NAME symbol_entry
#include "richc/template/array.h"


#endif // ifndef BARON_SYMBOLS_H_
