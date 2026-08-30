#ifndef BARON_MACROS_H_
#define BARON_MACROS_H_

#include "token.h"
#include "cursor.h"


// The macro store. A macro is a name bound to one or more OVERLOADS (signatures); each signature is a
// sequence of slots interleaving parameters and literal tokens, plus the cursor of the body it stamps
// out. The whole thing is rebuilt from source every pass (macros_reset), exactly like the section and
// include state, so an entry is a transient projection of the source, not durable state.
//
// A macro is NAMELESS here: its name lives in the dynamic statement-token table (on baron), and a
// lexeme_type_macro carries the index into list that reaches it. This is an internal element of its
// manager, addressed by index, never by a stored pointer (the list may relocate on growth).

// A signature slot: a parameter to bind, or a literal token to match verbatim. A tagged union - the two
// never coexist - discriminated by type.
typedef enum macro_slot_type {
    macro_slot_param,     // 0/default: binds a symbol of name in the expansion scope
    macro_slot_literal,   // matches a fixed token; literal_id selects which of the macro's literals
    macro_slot_comma,     // matches a bare comma (lexeme_type_comma); the conventional separator sugar
} macro_slot_type;

typedef struct macro_slot {
    uint32_t type;              // macro_slot_type - the discriminant of:
    union {
        rc_str   name;          // param:   the symbol name to bind
        uint32_t literal_id;    // literal: index into the owning macro's literal_table
    };
} macro_slot;

#define RC_ARRAY_TYPE macro_slot
#define RC_ARRAY_NAME macro_slot
#include "richc/template/array.h"

// One overload: its slot sequence, the body it expands to, and whether that body has actually been
// supplied. An empty body is a forward declaration (defined == false); a later matching definition fills
// it in. slots is a view into the manager's arena (the builder is promoted to a view on registration).
typedef struct macro_signature {
    rc_view_macro_slot slots;
    cursor             body;      // {source, pos} at the body start; ends at the ENDMACRO closer parse_block finds
    bool               defined;   // false = forward declaration (empty body); a real body later fills it in
} macro_signature;

#define RC_ARRAY_TYPE macro_signature
#define RC_ARRAY_NAME macro_signature
#include "richc/template/array.h"

// All the overloads sharing one name, plus that name's private matching table - one entry per distinct
// literal string, tagged with its literal_id (a lexeme_type_macro_literal). signatures is kept sorted
// so the invocation matcher can take the first full match (see macros_add_signature / the comparator).
typedef struct macro {
    rc_array_token           literal_table;
    rc_array_macro_signature signatures;
} macro;

#define RC_ARRAY_TYPE macro
#define RC_ARRAY_NAME macro
#include "richc/template/array.h"

// The manager: one arena backs everything (the list, each macro's sub-arrays, the promoted slot views,
// AND the dynamic statement-token table), all reset together each pass. Capacities are reserved generously
// up front so per-definition appends rarely reallocate within the shared arena.
//
// statement_tokens lives here, next to its arena: it is the static base table plus one lexeme_type_macro
// entry per macro name, rebuilt from source order every pass. The lexer reads it to recognise a name as a
// call; a name defined earlier this pass is in it, one defined later is not (definition order matters).
typedef struct macros {
    rc_arena      *arena;              // BORROWED: baron's per_pass arena (list, sub-arrays, tokens all live here)
    rc_array_macro list;               // one per distinct name; a lexeme_type_macro's index addresses it
    rc_array_token statement_tokens;   // the live dispatch table (static base + a token per macro name)
} macros;

void macros_init(macros *m, rc_arena *per_pass);

// Rebuild the store for a fresh pass and reseed statement_tokens from base (the static statement-token
// table) with room for reserve_extra macro-name tokens. The caller resets the shared per_pass arena
// once BEFORE this (which reclaims the previous pass's list, sub-arrays and tokens); this only re-makes
// the containers into the fresh arena.
void macros_reset(macros *m, token_table base, uint32_t reserve_extra);

// The live statement-token table the lexer dispatches from (the base plus a token per macro seen so far).
token_table macros_statement_tokens(const macros *m);

// Map a macro name to its list index, registering it on first sight: a new name appends a fresh (empty)
// macro AND a lexeme_type_macro token reaching it (so the body may already call the name); an existing name
// - an overload, a self-call, or a forward declaration being filled - reuses its index. name is a view
// into the (persistent) source, re-derived each pass, so it needs no copy.
uint32_t macros_index_for_name(macros *m, rc_str name);

// Append a fresh, empty macro (no signatures, empty literal table) and return its index. The caller
// pairs this with a statement-token entry carrying that index.
uint32_t macros_add(macros *m);

// A pointer to the macro at index. Valid only until the list next grows - copy out what you need
// (a signature, a body cursor) before anything that might register another macro.
macro *macros_at(macros *m, uint32_t index);

// Intern text as one of macro index's literals and return its literal_id (a stable per-macro id);
// an identical literal (case-insensitive, like every other token) returns the existing id.
uint32_t macros_intern_literal(macros *m, uint32_t index, rc_str text);

// The outcome of registering a signature on a name.
typedef enum macro_add_status {
    macro_add_inserted,    // a new overload (a new slot pattern)
    macro_add_filled,      // a real body supplied for a matching forward declaration
    macro_add_redundant,   // an empty (forward) body for an already-known pattern: a no-op
    macro_add_duplicate,   // a second real body for a pattern already defined
} macro_add_status;

// Register {slots, body, defined} on macro index, reconciling against its existing overloads by slot
// pattern (parameter names do not distinguish patterns; literal ids do). slots and body must outlive
// the pass (slots is promoted into the manager arena by the caller). See macro_add_status for outcomes.
macro_add_status macros_add_signature(macros *m, uint32_t index, rc_view_macro_slot slots, cursor body, bool defined);


#endif // ifndef BARON_MACROS_H_
