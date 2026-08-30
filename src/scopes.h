#ifndef BARON_SCOPES_H_
#define BARON_SCOPES_H_

#include "value.h"
#include "cursor.h"
#include "symbols.h"
#include "richc/hash.h"


// Scopes are the brace-delimited blocks in the source. A scope can be named by a preceding label
// (.routine { ... }), and from outside we reach its inner symbols by a dot-separated path
// (routine.core). Every scope owns a symbol map and a child-scope map, plus a link up to its
// lexically enclosing parent. The scopes container owns the whole tree; the individual scopes are
// scope_node records, which we hand around by index. Scope 0 is the root.

// A bound symbol: its current value plus the source position that defined it. The position is the
// binding's identity - the same statement re-walked on a later pass carries the same def (so we
// re-evaluate and watch for a moved value), while a different statement defining the same name in
// the same scope is a duplicate. Baron's source is immutable, so a name is bound exactly once.
typedef struct symbol {
    value  v;
    cursor def;
} symbol;

// The outcome of binding a symbol: the first enumerator is the no-op default.
typedef enum symbol_status {
    symbol_status_unchanged,   // brand new, or identical to last pass: no further pass needed
    symbol_status_changed,     // same definition, value moved: drives another pass
    symbol_status_duplicate,   // a different cursor already owns this name in this scope
} symbol_status;

// Both maps are keyed by name. We key on the rc_str by content - hashing and
// comparing the characters, not the pointer - so the same text spelled anywhere in
// the source lands on the same entry. The symbol map carries a symbol; the child
// map carries a uint32_t scope index.
#define RC_TRIE_KEY_TYPE   rc_str
#define RC_TRIE_VALUE_TYPE symbol
#define RC_TRIE_NAME       rc_trie_symbol
#define RC_TRIE_HASH(k)    rc_hash_str(k)
#define RC_TRIE_EQUAL(a,b) rc_str_is_equal((a), (b))
#include "richc/template/hash_trie.h"

#define RC_TRIE_KEY_TYPE   rc_str
#define RC_TRIE_VALUE_TYPE uint32_t
#define RC_TRIE_NAME       rc_trie_child
#define RC_TRIE_HASH(k)    rc_hash_str(k)
#define RC_TRIE_EQUAL(a,b) rc_str_is_equal((a), (b))
#include "richc/template/hash_trie.h"


// A single scope. Really an implementation detail of scopes: we keep these by
// index in the nodes array and pass the indices about, never the pointers.
typedef struct scope_node {
    rc_str         name;      // this scope's name; {0} (len 0) if anonymous/root
    uint32_t       parent;    // enclosing scope index; RC_INDEX_NONE for the root
    rc_trie_symbol symbols;   // symbol name -> symbol
    rc_trie_child  children;  // child scope name -> child scope index
} scope_node;

#define RC_ARRAY_TYPE scope_node
#define RC_ARRAY_NAME scope_node
#include "richc/template/array.h"


// The scope tree and symbol table rolled together: the scope_node array, the two shared trie pools,
// and (in arena) the deep-cloned symbol values and owned name keys. Everything is referenced by
// index - trie links are block indices into the shared pools, scopes by node index - so a growth
// that relocates a backing array to the arena top invalidates nothing, and a fully-built scopes is
// trivially copyable.
typedef struct scopes {
    rc_arena *arena;                  // BORROWED: baron's permanent arena, backs everything below
    rc_array_scope_node nodes;        // scope index 0 is the root
    rc_trie_symbol_pool symbol_pool;  // shared by every node's symbols trie
    rc_trie_child_pool  child_pool;   // shared by every node's children trie
} scopes;

// A read-only projection of a finished scopes: the scope-node array as a view, plus the two shared
// trie pools BY VALUE (richc pools have no view type, and a pool is itself a trivially-copyable
// read-only handle over arena memory; the read-only trie ops take a const POOL *). These three
// handles are all a lookup or a flatten needs. The backing lives in the permanent arena, so the view
// outlives the scopes struct it was projected from - it is valid as long as that arena is.
typedef struct scopes_view {
    rc_view_scope_node  nodes;        // scope index 0 is the root
    rc_trie_symbol_pool symbol_pool;  // read-only: backs every node's symbols trie
    rc_trie_child_pool  child_pool;   // read-only: backs every node's children trie
} scopes_view;

// Set-up. permanent is baron's permanent arena (not owned here). Filling a caller-owned struct is
// just the init convention, not a constraint: a fully-built scopes is freely movable.
void scopes_init(scopes *s, rc_arena *permanent);

// Empty the tree back to a bare root (index 0), reclaiming everything. Used to discard the
// half-built symbol table when an assemble fails, so no stale bindings are read afterwards.
void scopes_reset(scopes *s);

// Make the root scope (index 0). Call it once, before anything else.
uint32_t scopes_make_root(scopes *s);

// Make a child of parent_index and hand back its index. A non-empty name also
// registers the child in the parent's child map, so a path can find it later; an
// empty name ({0}) gives an anonymous scope that only its index can reach.
uint32_t scopes_make_child(scopes *s, uint32_t parent_index, rc_str name);

// Get the child of parent_index named name, making it if it does not exist yet.
// Idempotent, so re-walking the same source on a later pass lands on the same scope
// (and so keeps its symbol bindings) rather than spawning a duplicate. name is
// non-empty; on a first sighting its bytes are copied into the scopes' own arena, so
// the caller may pass a scratch view (an anonymous scope is given a synthetic key the
// caller builds, e.g. "@source:pos", which no user identifier can spell).
uint32_t scopes_get_or_make_child(scopes *s, uint32_t parent_index, rc_str name);

// Bind leaf name to v in scope_index, with def recording the source position
// that defines it - name is a plain symbol name, never a dotted path. The value is
// deep-cloned into the scopes' permanent arena (only on a genuine change), so a value
// built in a caller's scratch arena may be passed safely. The outcome:
//   - duplicate: a binding already exists under a DIFFERENT def - a second definition
//     of the same name in the same scope. Nothing is mutated; the caller errors.
//   - changed:   the same definition (matching def) re-evaluated to a different value.
//     This is what a later pass watches to decide whether things have settled.
//   - unchanged: a brand new symbol, or the same definition landing the same value again.
symbol_status scopes_set_symbol(scopes *s, uint32_t scope_index, rc_str name, value v, cursor def);

// Remove leaf name (a plain symbol name, not a path) from scope_index. Returns
// whether it was there to remove.
bool scopes_remove_symbol(scopes *s, uint32_t scope_index, rc_str name);

// The source position that defined leaf name in scope_index ALONE (no parent walk, no dotted
// path) - the def cursor scopes_set_symbol recorded - or cursor_none() if the name is not bound
// here. Used to point a duplicate-symbol error back at the binding it collides with.
cursor scopes_symbol_def(const scopes *s, uint32_t scope_index, rc_str name);

// A resolved binding's IDENTITY: its def cursor plus the scope it was declared in. The def cursor ALONE is
// not unique - a macro / FOR body shares one def across every instantiation - so the declaring scope, which
// the per-instantiation child scope makes distinct, is what tells two instances apart. def is cursor_none()
// and scope is RC_INDEX_NONE when the name is unbound.
typedef struct symbol_ref {
    cursor   def;
    uint32_t scope;
} symbol_ref;

// Resolve a name from scope_index with the same rules as scopes_get_symbol - a bare name walks up the parent
// chain (nearest enclosing definition wins), a dotted path (a.b.sym) descends the child scopes - but return
// the binding's identity (def + declaring scope) rather than its value. Used by the ZP allocator to map an
// operand to the exact variable instance it references, wherever it was declared.
symbol_ref scopes_resolve_symbol_def(const scopes *s, uint32_t scope_index, rc_str name);

// Look a symbol up, starting from scope_index. A bare name walks up the parent
// chain and takes the nearest enclosing definition. A dotted path (a.b.sym) finds
// its head the same way, then descends the rest strictly through the child maps
// and looks for the final name in the leaf scope alone. Hands back
// value_make_none() if nothing matches.
value scopes_get_symbol(const scopes *s, uint32_t scope_index, rc_str full_path);

// Resolve a LOCAL label reference (@- / @+) made at (source, use_pos) inside scope_index. Local labels are
// ordinary symbols bound under an unspellable "@source:pos" key; we scan this scope's symbols ALONE (no parent
// walk - locals do not leak across scopes), keep the '@'-prefixed keys defined in the same source, and pick
// the nearest by DEFINITION position: the largest def.pos < use_pos for a backward '@-' (forward == false), or
// the smallest def.pos > use_pos for a forward '@+' (forward == true). Ordering is by source position, never
// by value, so an ORG between two local labels cannot reorder them. Hands back the winner's value, or an
// unknown-symbol error value when none qualifies (so an unresolved @- / @+ defers like any forward reference).
value scopes_find_local_label(const scopes *s, uint32_t scope_index, uint32_t source, uint32_t use_pos, bool forward);

// Project a finished scopes into a read-only view (see scopes_view above). Cheap - it copies the nodes
// view and the two pool handles; the arena-backed backing is shared, not duplicated.
scopes_view scopes_view_make(const scopes *s);

// Look a full dotted path up in the view, from the top level (root). A bare name ("top") is found in the
// root scope; a dotted path ("routine.core") descends the child maps and reads the leaf scope's symbol map.
// Hands back value_make_none() if nothing matches. (This is the result-facing lookup; the internal
// scopes_get_symbol adds a starting scope + parent walk for use during assembly.)
value scopes_view_get_symbol(scopes_view v, rc_str full_path);

// Flatten every spellable resolved binding into a fresh array (backed by arena) and hand back its view,
// each entry keyed by its full dotted path from the top level ("routine.core"). Unspellable internals are
// skipped: any scope or symbol whose name begins with '@' - anonymous { } blocks (a synthetic "@source:pos"
// key), FOR-iteration and macro/function call frames, and local labels - none of which a source path can
// spell or scopes_view_get_symbol can reach. A top-level symbol's path reuses its owned key rc_str directly (no
// copy); a nested symbol's path is built in arena. scratch backs the per-scope prefix while it is
// assembled. The result is a read-only, position-independent snapshot (paths and values all live in arena),
// so it outlives the view.
rc_view_symbol_entry scopes_view_flatten(scopes_view v, rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_SCOPES_H_
