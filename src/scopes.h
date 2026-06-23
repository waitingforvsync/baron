#ifndef BARON_SCOPES_H_
#define BARON_SCOPES_H_

#include "value.h"
#include "richc/hash.h"


// Scopes are the brace-delimited blocks in the source. A scope can be named by a
// preceding label (.routine { ... }), and from outside we reach its inner symbols
// by a dot-separated path (routine.core). Every scope owns a symbol map (name ->
// value) and a child-scope map (name -> child index), plus a link up to its
// lexically enclosing parent. The `scopes` container owns the whole tree; the
// individual scopes are `scope_node` records, which we hand around by index. Scope
// 0 is the root.

// Both maps are keyed by name. We key on the rc_str by content - hashing and
// comparing the characters, not the pointer - so the same text spelled anywhere in
// the source lands on the same entry. The symbol map carries a `value`; the child
// map carries a uint32_t scope index.
#define RC_TRIE_KEY_TYPE   rc_str
#define RC_TRIE_VALUE_TYPE value
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


// A single scope. Really an implementation detail of `scopes`: we keep these by
// index in the nodes array and pass the indices about, never the pointers.
typedef struct scope_node {
    rc_str         name;      // this scope's name; {0} (len 0) if anonymous/root
    uint32_t       parent;    // enclosing scope index; RC_INDEX_NONE for the root
    rc_trie_symbol symbols;   // symbol name -> value
    rc_trie_child  children;  // child scope name -> child scope index
} scope_node;

// rc_view/span/array_scope_node. scope_node is not recursive, so one go does it.
#define RC_ARRAY_TYPE scope_node
#define RC_ARRAY_NAME scope_node
#include "richc/template/array.h"


// The scope tree and symbol table rolled together. It owns every growable thing:
// the scope_node array and the two trie node pools, each parked on its own arena
// so it can grow in place as the sole tenant. All scopes share the one symbol_pool
// and the one child_pool (a single pool happily backs many tries). Keep it put
// once it holds scopes - the per-node tries squirrel away pointers back into
// symbol_pool / child_pool.
typedef struct scopes {
    rc_arena node_arena;     // backs `nodes`
    rc_arena symbol_arena;   // backs `symbol_pool`
    rc_arena child_arena;    // backs `child_pool`
    rc_array_scope_node nodes;        // scope index 0 is the root
    rc_trie_symbol_pool symbol_pool;  // shared by every node's `symbols` trie
    rc_trie_child_pool  child_pool;   // shared by every node's `children` trie
} scopes;

// Set-up and tear-down. In-place, because once the container holds scopes it must
// not budge.
void scopes_init(scopes *s);
void scopes_deinit(scopes *s);

// Make the root scope (index 0). Call it once, before anything else.
uint32_t scopes_make_root(scopes *s);

// Make a child of parent_index and hand back its index. A non-empty `name` also
// registers the child in the parent's child map, so a path can find it later; an
// empty name ({0}) gives an anonymous scope that only its index can reach.
uint32_t scopes_make_child(scopes *s, uint32_t parent_index, rc_str name);

// Set leaf `name` to `v` in scope_index - `name` is a plain symbol name, never a
// dotted path. Returns whether we changed an existing binding: true only if the
// symbol was already defined and its old value differs from `v`. A brand new
// symbol (or a write that lands the same value again) returns false. Either way
// `name` ends up bound to `v`. The "did it change" answer is what a later pass
// watches to decide whether things have settled.
bool scopes_set_symbol(scopes *s, uint32_t scope_index, rc_str name, value v);

// Remove leaf `name` (a plain symbol name, not a path) from scope_index. Returns
// whether it was there to remove.
bool scopes_remove_symbol(scopes *s, uint32_t scope_index, rc_str name);

// Look a symbol up, starting from scope_index. A bare name walks up the parent
// chain and takes the nearest enclosing definition. A dotted path (a.b.sym) finds
// its head the same way, then descends the rest strictly through the child maps
// and looks for the final name in the leaf scope alone. Hands back
// value_make_none() if nothing matches.
value scopes_get_symbol(const scopes *s, uint32_t scope_index, rc_str full_path);


#endif // ifndef BARON_SCOPES_H_
