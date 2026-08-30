#include "scopes.h"

#include "richc/macros.h"
#include "richc/mstr.h"


// A symbol or scope name is a single identifier: it must carry no path separator.
static bool is_leaf_name(rc_str name)
{
    return rc_str_find_first(name, RC_STR(".")) == RC_INDEX_NONE;
}

// Up-front reserves, sized for a big 6502 project. Named scopes are few; the count is dominated by
// scopes with unspellable names - one per FOR iteration and per function / macro call frame - and every
// scope allocates one symbol block AND one child block on creation, so all three track scope count.
// Growth past these is safe (indices), just a copy.
enum {
    scopes_nodes_reserve         = 4096,
    scopes_symbol_blocks_reserve = 4096,
    scopes_child_blocks_reserve  = 4096,
};

void scopes_init(scopes *s, rc_arena *permanent)
{
    RC_ASSERT(s != NULL && permanent != NULL);
    s->arena       = permanent;   // borrowed; baron owns it
    s->nodes       = rc_array_scope_node_make(scopes_nodes_reserve, s->arena);
    s->symbol_pool = rc_trie_symbol_pool_make(scopes_symbol_blocks_reserve, s->arena);
    s->child_pool  = rc_trie_child_pool_make(scopes_child_blocks_reserve, s->arena);
}

void scopes_reset(scopes *s)
{
    RC_ASSERT(s != NULL);
    // Discard the half-built tree by standing up fresh, empty containers in the permanent arena (a bare
    // root at index 0). The old nodes / pool blocks / values become holes - acceptable on this
    // failure-only path. We do NOT reset the arena: it is shared (source text, diagnostics live there too).
    s->nodes       = rc_array_scope_node_make(scopes_nodes_reserve, s->arena);
    s->symbol_pool = rc_trie_symbol_pool_make(scopes_symbol_blocks_reserve, s->arena);
    s->child_pool  = rc_trie_child_pool_make(scopes_child_blocks_reserve, s->arena);
    scopes_make_root(s);
}

uint32_t scopes_make_root(scopes *s)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(rc_array_scope_node_is_empty(&s->nodes));   // the root is the first node

    // A node's tries start empty (zero-init: root == 0 means "no root block yet"); the first symbol / child
    // added to a scope lazily allocates its root block in the shared pool. No construction ceremony needed.
    return rc_array_scope_node_push(
        &s->nodes,
        (scope_node) {
            .name   = (rc_str) {0},
            .parent = RC_INDEX_NONE,
        },
        s->arena);
}

uint32_t scopes_make_child(scopes *s, uint32_t parent_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    // A real (source) child name is a single identifier; a synthetic anonymous-scope key the caller
    // builds ('@source:pos...') deliberately is not, so we only insist the bytes outlive the scopes.

    uint32_t child_index = rc_array_scope_node_push(
        &s->nodes,
        (scope_node) {
            .name   = name,
            .parent = parent_index,
        },
        s->arena);

    // Reach the parent fresh after the push: it may have shuffled the nodes array
    // along. An anonymous child is reachable only by index, so we leave it off the
    // child map.
    if (name.len > 0) {
        rc_trie_child_add(
            &RC_AT(s->nodes, parent_index).children,
            &s->child_pool,
            name,
            child_index,
            s->arena);
    }
    return child_index;
}

uint32_t scopes_get_or_make_child(scopes *s, uint32_t parent_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(name.len > 0);

    rc_trie_child kids  = RC_AT(s->nodes, parent_index).children;
    uint32_t      found = rc_trie_child_find(kids, &s->child_pool, name);   // probe with the caller's (maybe scratch) view
    if (found != RC_INDEX_NONE) {
        return rc_trie_child_value_get(&s->child_pool, found);
    }

    // First sighting: own the key in the permanent arena so the caller may hand us a scratch buffer.
    rc_str owned = rc_mstr_from_str(name, name.len, s->arena).view;
    return scopes_make_child(s, parent_index, owned);
}

symbol_status scopes_set_symbol(scopes *s, uint32_t scope_index, rc_str name, value v, cursor def)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(is_leaf_name(name));

    rc_trie_symbol *syms  = &RC_AT(s->nodes, scope_index).symbols;
    uint32_t        found = rc_trie_symbol_find(*syms, &s->symbol_pool, name);
    if (found != RC_INDEX_NONE) {
        symbol existing = rc_trie_symbol_value_get(&s->symbol_pool, found);
        // A different definition reaching the same name in the same scope is a duplicate -
        // immutable source means a name is bound exactly once. Leave the binding untouched.
        if (!cursor_is_equal(existing.def, def)) {
            return symbol_status_duplicate;
        }
        // The same definition, re-walked on a later pass: skip the clone when the value has
        // not actually shifted, both to answer the convergence question and to keep the value
        // arena from growing every pass. Only a genuine change pays for fresh permanent backing.
        if (value_is_equal(existing.v, v)) {
            return symbol_status_unchanged;
        }
        rc_trie_symbol_value_set(&s->symbol_pool, found, (symbol){ value_make_copy(v, s->arena), def });
        return symbol_status_changed;
    }
    // First binding: own the key in the permanent arena too, so the caller may hand us a scratch view - a
    // local label's synthetic '@source:pos' key has no backing in the source text. Only the add path copies
    // (a later pass finds the key by content and reuses it), so this is one copy per symbol, not per pass.
    rc_str owned = rc_mstr_from_str(name, name.len, s->arena).view;
    rc_trie_symbol_add(syms, &s->symbol_pool, owned, (symbol){ value_make_copy(v, s->arena), def }, s->arena);
    return symbol_status_unchanged;   // brand new, so there was nothing to converge from
}

bool scopes_remove_symbol(scopes *s, uint32_t scope_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(is_leaf_name(name));
    return rc_trie_symbol_delete(RC_AT(s->nodes, scope_index).symbols, &s->symbol_pool, name);
}

cursor scopes_symbol_def(const scopes *s, uint32_t scope_index, rc_str name)
{
    RC_ASSERT(s != NULL && is_leaf_name(name));

    rc_trie_symbol syms  = RC_AT(s->nodes, scope_index).symbols;
    uint32_t       found = rc_trie_symbol_find(syms, &s->symbol_pool, name);
    return found == RC_INDEX_NONE
               ? cursor_none()
               : rc_trie_symbol_value_get(&s->symbol_pool, found).def;
}

scopes_view scopes_view_make(const scopes *s)
{
    RC_ASSERT(s != NULL);
    return (scopes_view) {
        .nodes       = s->nodes.view,
        .symbol_pool = s->symbol_pool,
        .child_pool  = s->child_pool,
    };
}

// Where the lookup core found a binding: the scope it lives in and its index in the symbol pool
// (both RC_INDEX_NONE when nothing matched). Enough to read the value or the def off one find.
typedef struct symbol_loc {
    uint32_t scope;
    uint32_t index;
} symbol_loc;

// The shared lookup core, over a read-only view. From scope_index: a bare name walks up the parent chain
// (nearest enclosing definition wins); a dotted path finds its head the same way, then descends the rest
// strictly through the child maps and reads the final name in the leaf scope alone.
static symbol_loc view_find_from(scopes_view v, uint32_t scope_index, rc_str full_path)
{
    symbol_loc  none  = {.scope = RC_INDEX_NONE, .index = RC_INDEX_NONE};
    rc_str      dot   = RC_STR(".");
    rc_str_pair split = rc_str_first_split(full_path, dot);

    // Bare name: walk up the parent chain, and the nearest enclosing definition wins.
    if (!rc_str_is_valid(split.second)) {
        for (uint32_t i = scope_index; i != RC_INDEX_NONE; i = rc_view_scope_node_get(v.nodes, i).parent) {
            rc_trie_symbol syms  = rc_view_scope_node_get(v.nodes, i).symbols;
            uint32_t       found = rc_trie_symbol_find(syms, &v.symbol_pool, full_path);
            if (found != RC_INDEX_NONE) {
                return (symbol_loc) {.scope = i, .index = found};
            }
        }
        return none;
    }

    // Dotted path: find the head the same way (walk up the parents), then descend
    // the rest strictly through the child maps.
    uint32_t cur  = RC_INDEX_NONE;
    rc_str   head = split.first;
    for (uint32_t i = scope_index; i != RC_INDEX_NONE; i = rc_view_scope_node_get(v.nodes, i).parent) {
        rc_trie_child kids  = rc_view_scope_node_get(v.nodes, i).children;
        uint32_t      found = rc_trie_child_find(kids, &v.child_pool, head);
        if (found != RC_INDEX_NONE) {
            cur = rc_trie_child_value_get(&v.child_pool, found);
            break;
        }
    }
    if (cur == RC_INDEX_NONE) {
        return none;
    }

    rc_str rest = split.second;
    while (true) {
        split = rc_str_first_split(rest, dot);
        if (!rc_str_is_valid(split.second)) {
            // Last component: the symbol name, looked up in the leaf scope alone.
            rc_trie_symbol syms  = rc_view_scope_node_get(v.nodes, cur).symbols;
            uint32_t       found = rc_trie_symbol_find(syms, &v.symbol_pool, rest);
            return found == RC_INDEX_NONE ? none : (symbol_loc) {.scope = cur, .index = found};
        }
        // Intermediate component: step down into the named child scope.
        rc_trie_child kids  = rc_view_scope_node_get(v.nodes, cur).children;
        uint32_t      found = rc_trie_child_find(kids, &v.child_pool, split.first);
        if (found == RC_INDEX_NONE) {
            return none;
        }
        cur  = rc_trie_child_value_get(&v.child_pool, found);
        rest = split.second;
    }
}

// The value at a lookup's result, or value_make_none() when nothing matched.
static value view_get_from(scopes_view v, uint32_t scope_index, rc_str full_path)
{
    symbol_loc loc = view_find_from(v, scope_index, full_path);
    return loc.index == RC_INDEX_NONE ? value_make_none() : rc_trie_symbol_value_get(&v.symbol_pool, loc.index).v;
}

symbol_ref scopes_resolve_symbol_def(const scopes *s, uint32_t scope_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    // The same walk as a value lookup - bare names shadow up the parent chain, dotted paths descend the
    // child scopes - but we return the binding's def (its identity) plus the scope it was found in: the
    // two together uniquely identify a variable across instantiations.
    scopes_view v   = scopes_view_make(s);
    symbol_loc  loc = view_find_from(v, scope_index, name);
    if (loc.index == RC_INDEX_NONE) {
        return (symbol_ref) {.def = cursor_none(), .scope = RC_INDEX_NONE};
    }
    return (symbol_ref) {.def = rc_trie_symbol_value_get(&v.symbol_pool, loc.index).def, .scope = loc.scope};
}

// Look a symbol up during assembly, starting from scope_index (with parent-walk shadowing).
value scopes_get_symbol(const scopes *s, uint32_t scope_index, rc_str full_path)
{
    RC_ASSERT(s != NULL);
    return view_get_from(scopes_view_make(s), scope_index, full_path);
}

// Result-facing lookup: a full dotted path from the top level (root == scope index 0).
value scopes_view_get_symbol(scopes_view v, rc_str full_path)
{
    return view_get_from(v, 0, full_path);
}


// The running state of a local-label scan: which reference we are resolving, and the best candidate so far.
typedef struct local_label_search {
    uint32_t source;    // the reference's source; a candidate in another source is not comparable
    uint32_t use_pos;   // the reference's position; we want the nearest label either side of it
    bool     forward;   // '@+' (nearest label after) when true, '@-' (nearest before) when false
    bool     found;     // whether `result` / `best_pos` hold a candidate yet
    uint32_t best_pos;  // the def position of the best candidate so far
    value    result;    // that candidate's value
} local_label_search;

// One trie entry: keep it only if it is a local label ('@'-prefixed key) defined in the same source, then
// let it displace the current best when it is nearer to the reference on the correct side. We compare by the
// DEFINITION position (sym.def.pos), never by the value, so an ORG between two labels cannot reorder them.
static void local_label_visit(local_label_search *search, const rc_trie_symbol_pool *pool, uint32_t index)
{
    rc_str key = rc_trie_symbol_key_get(pool, index);
    if (!rc_str_starts_with(key, RC_STR("@"))) {
        return;   // an ordinary named symbol, not a local label
    }
    symbol sym = rc_trie_symbol_value_get(pool, index);
    if (sym.def.source != search->source) {
        return;   // defined in a different source: positions are not comparable
    }
    uint32_t pos = sym.def.pos;
    bool nearer = search->forward
                      ? (pos > search->use_pos && (!search->found || pos < search->best_pos))
                      : (pos < search->use_pos && (!search->found || pos > search->best_pos));
    if (nearer) {
        search->found    = true;
        search->best_pos = pos;
        search->result   = sym.v;
    }
}

#define RC_TRIE_FOREACH_TRIE          rc_trie_symbol
#define RC_TRIE_FOREACH_CTX           local_label_search
#define RC_TRIE_FOREACH_CONST                              // read-only scan (const scopes)
#define RC_TRIE_FOREACH_FUNC(c, p, i) local_label_visit(c, p, i)
#include "richc/template/algorithm/hash_trie_foreach.h"   // rc_trie_symbol_foreach

value scopes_find_local_label(const scopes *s, uint32_t scope_index, uint32_t source, uint32_t use_pos, bool forward)
{
    RC_ASSERT(s != NULL);
    local_label_search search = {
        .source  = source,
        .use_pos = use_pos,
        .forward = forward,
    };
    rc_trie_symbol_foreach(RC_AT(s->nodes, scope_index).symbols, &s->symbol_pool, &search);
    // No candidate: report it as an unknown symbol, so an unresolved @- / @+ defers like any forward
    // reference (and becomes undefined_symbol on the final pass if it never binds).
    return search.found ? search.result : value_make_error(error_type_unknown_symbol);
}


// Scope `i`'s dotted prefix (root-to-leaf, each name followed by '.'), plus whether it can be spelled at all.
// `spellable` is false if any scope on the path (i itself or an ancestor) is unspellable - its name begins
// '@' (an anonymous block / call frame / iteration scope) - meaning its symbols cannot be reached by a source
// path and the whole scope should be skipped. The root (empty name) contributes nothing, so a top-level scope
// gets an empty prefix.
typedef struct scope_prefix {
    rc_str prefix;
    bool   spellable;
} scope_prefix;

// Build that prefix by value, recursing to the parent first so the segments land in reading order. The
// intermediate strings pile up in `scratch`.
static scope_prefix build_scope_prefix(rc_view_scope_node nodes, uint32_t i, rc_arena *scratch)
{
    if (i == RC_INDEX_NONE) {
        return (scope_prefix) {.prefix = RC_STR(""), .spellable = true};   // walked off the top of the root
    }
    scope_node   node   = rc_view_scope_node_get(nodes, i);
    scope_prefix parent = build_scope_prefix(nodes, node.parent, scratch);
    if (!parent.spellable) {
        return parent;   // already doomed by an ancestor
    }
    if (node.name.len == 0) {
        return parent;   // the root: no segment of its own
    }
    if (rc_str_starts_with(node.name, RC_STR("@"))) {
        return (scope_prefix) {.prefix = RC_STR(""), .spellable = false};
    }
    rc_mstr m = rc_mstr_make(parent.prefix.len + node.name.len + 1, scratch);
    rc_mstr_append(&m, parent.prefix, scratch);
    rc_mstr_append(&m, node.name, scratch);
    rc_mstr_append_char(&m, '.', scratch);
    return (scope_prefix) {.prefix = m.view, .spellable = true};
}

// The accumulator for one scope's flatten pass: where entries go, the arena their paths grow in, and this
// scope's already-built prefix ("" at the top level, "routine." one level in).
typedef struct symbol_flatten {
    rc_array_symbol_entry *out;
    rc_arena              *arena;
    rc_str                 prefix;
} symbol_flatten;

// One symbol of the current scope: skip the unspellable '@' local labels, otherwise record it under its full
// path. A top-level name (empty prefix) is stored by reference to its owned key - no copy; a nested name is
// prefixed into `arena` (the prefix already carries its trailing '.').
static void flatten_symbol(symbol_flatten *c, const rc_trie_symbol_pool *pool, uint32_t i)
{
    rc_str name = rc_trie_symbol_key_get(pool, i);
    if (rc_str_starts_with(name, RC_STR("@"))) {
        return;   // a local label bound under an unspellable key
    }
    rc_str path = name;
    if (c->prefix.len != 0) {
        rc_mstr full = rc_mstr_make(c->prefix.len + name.len, c->arena);
        rc_mstr_append(&full, c->prefix, c->arena);
        rc_mstr_append(&full, name, c->arena);
        path = full.view;
    }
    rc_array_symbol_entry_push(c->out,
        (symbol_entry) {.path = path, .v = rc_trie_symbol_value_get(pool, i).v}, c->arena);
}

#define RC_TRIE_FOREACH_TRIE          rc_trie_symbol
#define RC_TRIE_FOREACH_CTX           symbol_flatten
#define RC_TRIE_FOREACH_CONST                              // read-only scan (const pool)
#define RC_TRIE_FOREACH_FUNC(c, p, i) flatten_symbol(c, p, i)
#define RC_TRIE_FOREACH_NAME          rc_trie_symbol_foreach_flatten   // the local-label one already owns the default name
#include "richc/template/algorithm/hash_trie_foreach.h"

rc_view_symbol_entry scopes_view_flatten(scopes_view v, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena != NULL);

    rc_array_symbol_entry out = rc_array_symbol_entry_make(64, arena);
    symbol_flatten        ctx = { .out = &out, .arena = arena };
    for (uint32_t i = 0; i < v.nodes.num; i++) {
        scope_prefix sp = build_scope_prefix(v.nodes, i, &scratch);
        if (!sp.spellable) {
            continue;   // an unspellable scope: none of its symbols are reachable by path
        }
        ctx.prefix = sp.prefix;
        rc_trie_symbol_foreach_flatten(rc_view_scope_node_get(v.nodes, i).symbols, &v.symbol_pool, &ctx);
    }
    return out.view;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Each test gets a fresh scope tree with its root already made.
RC_TEST_GROUP_DATA(scopes) {
    rc_arena arena;
    scopes   scopes;
    uint32_t root;
};

RC_TEST_GROUP_INIT(scopes, fix)
{
    fix->arena = rc_arena_make_default();
    scopes_init(&fix->scopes, &fix->arena);
    fix->root = scopes_make_root(&fix->scopes);
}

RC_TEST_GROUP_DEINIT(scopes, fix)
{
    rc_arena_deinit(&fix->arena);
}

RC_TEST_STEP(scopes, set_get_overwrite, fix)
{
    // The same definition re-evaluated across passes: one source position throughout.
    cursor pos = { 0, 10 };

    // A brand new symbol reports no change; get hands it straight back.
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, fix->root, RC_STR("snowy"), value_make_numeric(1234.0), pos) == symbol_status_unchanged);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("snowy")), value_make_numeric(1234.0)));

    // Rewriting the same value from the same definition is not a change.
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, fix->root, RC_STR("snowy"), value_make_numeric(1234.0), pos) == symbol_status_unchanged);

    // The same definition landing a different value is a change, and it sticks.
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, fix->root, RC_STR("snowy"), value_make_numeric(321.0), pos) == symbol_status_changed);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("snowy")), value_make_numeric(321.0)));

    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("penguin"))));
}

RC_TEST_STEP(scopes, duplicate_symbol, fix)
{
    // First definition of `dup` takes; a SECOND definition (different source position) in the
    // same scope is a duplicate and leaves the original binding untouched.
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, fix->root, RC_STR("dup"), value_make_numeric(1.0), (cursor){0, 5}) == symbol_status_unchanged);
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, fix->root, RC_STR("dup"), value_make_numeric(2.0), (cursor){0, 9}) == symbol_status_duplicate);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("dup")), value_make_numeric(1.0)));

    // The same name in a CHILD scope is fine - inner scopes legitimately mirror outer names.
    uint32_t child = scopes_make_child(&fix->scopes, fix->root, RC_STR("inner"));
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, child, RC_STR("dup"), value_make_numeric(3.0), (cursor){0, 9}) == symbol_status_unchanged);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, child, RC_STR("dup")), value_make_numeric(3.0)));
}

RC_TEST_STEP(scopes, find_local_label, fix)
{
    scopes  *sc   = &fix->scopes;
    uint32_t root = fix->root;

    // Three local labels in one scope under the unspellable "@source:pos" keys, defined at ascending
    // positions. Their VALUES are deliberately not in position order (an ORG could do this), to prove the
    // scan orders by the definition cursor, not by the value. A plain symbol at pos 25 must be ignored.
    scopes_set_symbol(sc, root, RC_STR("@0:10"), value_make_numeric(0x2000), (cursor){0, 10});
    scopes_set_symbol(sc, root, RC_STR("@0:20"), value_make_numeric(0x1000), (cursor){0, 20});
    scopes_set_symbol(sc, root, RC_STR("@0:30"), value_make_numeric(0x3000), (cursor){0, 30});
    scopes_set_symbol(sc, root, RC_STR("plain"), value_make_numeric(0x9999), (cursor){0, 25});

    // From pos 25: nearest-before (@-) is @0:20 (value 0x1000); nearest-after (@+) is @0:30 (0x3000).
    RC_CHECK_TRUE(value_is_equal(scopes_find_local_label(sc, root, 0, 25, false), value_make_numeric(0x1000)));
    RC_CHECK_TRUE(value_is_equal(scopes_find_local_label(sc, root, 0, 25, true),  value_make_numeric(0x3000)));

    // From pos 15: @- is @0:10 (0x2000), @+ is @0:20 (0x1000) - lower value, but the next one along.
    RC_CHECK_TRUE(value_is_equal(scopes_find_local_label(sc, root, 0, 15, false), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(scopes_find_local_label(sc, root, 0, 15, true),  value_make_numeric(0x1000)));

    // Before the first (@-) and after the last (@+): nothing qualifies -> an unknown-symbol error value.
    RC_CHECK_TRUE(value_is_error(scopes_find_local_label(sc, root, 0, 5,  false)));
    RC_CHECK_TRUE(value_is_error(scopes_find_local_label(sc, root, 0, 35, true)));

    // A reference in a different source finds no comparable candidates here.
    RC_CHECK_TRUE(value_is_error(scopes_find_local_label(sc, root, 1, 25, false)));
}

RC_TEST_STEP(scopes, remove, fix)
{
    scopes_set_symbol(&fix->scopes, fix->root, RC_STR("barn"), value_make_numeric(3141.0), (cursor){0, 20});
    RC_CHECK_TRUE(scopes_remove_symbol(&fix->scopes, fix->root, RC_STR("barn")));    // was present
    RC_CHECK_FALSE(scopes_remove_symbol(&fix->scopes, fix->root, RC_STR("barn")));   // now gone
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("barn"))));
}

RC_TEST_STEP(scopes, qualified_path, fix)
{
    uint32_t routine = scopes_make_child(&fix->scopes, fix->root, RC_STR("routine"));

    scopes_set_symbol(&fix->scopes, routine, RC_STR("core"), value_make_numeric(0x2000), (cursor){0, 30});

    // Bare name from inside the child, and the dotted path from the parent.
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, routine, RC_STR("core")), value_make_numeric(0x2000)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("routine.core")), value_make_numeric(0x2000)));

    // An unknown leaf symbol and an unknown head scope both come up empty.
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("routine.missing"))));
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("ghost.core"))));
}

RC_TEST_STEP(scopes, qualified_head_walks_up, fix)
{
    // From a sibling scope, routine.core still finds `routine` by walking up to the root.
    uint32_t routine = scopes_make_child(&fix->scopes, fix->root, RC_STR("routine"));
    uint32_t other   = scopes_make_child(&fix->scopes, fix->root, RC_STR("other"));

    scopes_set_symbol(&fix->scopes, routine, RC_STR("core"), value_make_numeric(42.0), (cursor){0, 40});

    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, other, RC_STR("routine.core")), value_make_numeric(42.0)));
}

RC_TEST_STEP(scopes, resolve_def_follows_dotted_paths, fix)
{
    // The identity lookup obeys the same rules as the value lookup: a bare name shadows up the parents,
    // a dotted path descends the child scopes - and it reports the LEAF scope as the declaring one, which
    // is what makes a cross-scope ZA_AUTO operand resolve to the exact variable instance.
    uint32_t routine = scopes_make_child(&fix->scopes, fix->root, RC_STR("routine"));
    uint32_t other   = scopes_make_child(&fix->scopes, fix->root, RC_STR("other"));

    scopes_set_symbol(&fix->scopes, routine, RC_STR("core"), value_make_numeric(0x70), (cursor){0, 30});

    symbol_ref ref = scopes_resolve_symbol_def(&fix->scopes, other, RC_STR("routine.core"));
    RC_CHECK_TRUE(cursor_is_equal(ref.def, (cursor){0, 30}));
    RC_CHECK(ref.scope, ==, routine);

    // The dotted and bare routes land on the SAME identity.
    symbol_ref bare = scopes_resolve_symbol_def(&fix->scopes, routine, RC_STR("core"));
    RC_CHECK_TRUE(cursor_is_equal(bare.def, ref.def));
    RC_CHECK(bare.scope, ==, ref.scope);

    // Unknown leaf and unknown head both come up empty.
    RC_CHECK_TRUE(cursor_is_none(scopes_resolve_symbol_def(&fix->scopes, other, RC_STR("routine.missing")).def));
    RC_CHECK_TRUE(cursor_is_none(scopes_resolve_symbol_def(&fix->scopes, other, RC_STR("ghost.core")).def));
}

RC_TEST_STEP(scopes, nested_path_and_shadowing, fix)
{
    uint32_t a = scopes_make_child(&fix->scopes, fix->root, RC_STR("a"));
    uint32_t b = scopes_make_child(&fix->scopes, a, RC_STR("b"));

    scopes_set_symbol(&fix->scopes, fix->root, RC_STR("x"), value_make_numeric(1.0),  (cursor){0, 50});
    scopes_set_symbol(&fix->scopes, b,         RC_STR("x"), value_make_numeric(2.0),  (cursor){0, 60});
    scopes_set_symbol(&fix->scopes, b,         RC_STR("y"), value_make_numeric(99.0), (cursor){0, 70});

    // Three-component descent from the root.
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("a.b.y")), value_make_numeric(99.0)));
    // Shadowing: from b a bare x is b's; from a it falls through to the root's.
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, b, RC_STR("x")), value_make_numeric(2.0)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, a, RC_STR("x")), value_make_numeric(1.0)));
}

RC_TEST_STEP(scopes, set_symbol_clones_into_permanent, fix)
{
    // A value built in a throwaway arena must survive that arena being wiped, because
    // scopes_set_symbol deep-copies it into the scopes' own value arena.
    rc_arena scratch = rc_arena_make_default();
    rc_mstr  m = rc_mstr_make(8, &scratch);
    rc_mstr_append(&m, RC_STR("zip"), &scratch);
    RC_CHECK_TRUE(scopes_set_symbol(&fix->scopes, fix->root, RC_STR("s"), value_make_string(m.view), (cursor){0, 80}) == symbol_status_unchanged);

    rc_arena_reset(&scratch);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("s")),
                                 value_make_string(RC_STR("zip"))));
    rc_arena_deinit(&scratch);
}

RC_TEST_STEP(scopes, copy_by_value_still_resolves, fix)
{
    // The payoff of the value-trie redesign: no node holds a pointer into the scopes struct any more (a trie
    // is now just an index into a shared pool), so a fully-built scopes copied BY VALUE resolves symbols
    // identically to the original - the copy's tries index the same pool blocks. This is what makes a
    // scopes (hence baron) safe to hand around / return by value once it is done being built.
    uint32_t routine = scopes_make_child(&fix->scopes, fix->root, RC_STR("routine"));
    scopes_set_symbol(&fix->scopes, fix->root, RC_STR("top"),  value_make_numeric(11.0), (cursor){0, 1});
    scopes_set_symbol(&fix->scopes, routine,   RC_STR("core"), value_make_numeric(22.0), (cursor){0, 2});

    scopes copy = fix->scopes;   // a bitwise copy: just the nodes array header + two pool headers, all indices

    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&copy, fix->root, RC_STR("top")),          value_make_numeric(11.0)));
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&copy, fix->root, RC_STR("routine.core")), value_make_numeric(22.0)));
    RC_CHECK_TRUE(value_is_none(scopes_get_symbol(&copy, fix->root, RC_STR("ghost"))));
}

RC_TEST_STEP(scopes, get_or_make_child_is_idempotent, fix)
{
    // Re-walking the same source lands on the same scope, for a real name and for a synthetic
    // anonymous key (which the caller now builds; '@source:pos' is not a leaf name, so this also
    // exercises the relaxed-assert path).
    uint32_t a = scopes_get_or_make_child(&fix->scopes, fix->root, RC_STR("blk"));
    RC_CHECK(scopes_get_or_make_child(&fix->scopes, fix->root, RC_STR("blk")), ==, a);

    uint32_t p = scopes_get_or_make_child(&fix->scopes, fix->root, RC_STR("@0:42"));
    RC_CHECK(scopes_get_or_make_child(&fix->scopes, fix->root, RC_STR("@0:42")), ==, p);
    RC_CHECK_TRUE(p != scopes_get_or_make_child(&fix->scopes, fix->root, RC_STR("@0:99")));   // different site, different scope

    // Bindings in the reused scope persist (this is what keeps multi-pass convergence honest).
    scopes_set_symbol(&fix->scopes, a, RC_STR("inner"), value_make_numeric(7), (cursor){0, 90});
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&fix->scopes, fix->root, RC_STR("blk.inner")),
                                 value_make_numeric(7)));
}

// The value the flattened table holds for `path`, or none if it is absent (a plain scan of the snapshot).
static value flattened_lookup(rc_view_symbol_entry entries, rc_str path)
{
    for (uint32_t i = 0; i < entries.num; i++) {
        symbol_entry e = rc_view_symbol_entry_get(entries, i);
        if (rc_str_is_equal(e.path, path)) {
            return e.v;
        }
    }
    return value_make_none();
}

RC_TEST_STEP(scopes, flatten_snapshots_spellable_symbols, fix)
{
    // A top-level symbol, a symbol in a named child (reachable as "routine.core"), a symbol in a nameless
    // anonymous scope (synthetic '@' key), and a local label ('@' symbol) at the top level.
    uint32_t routine = scopes_make_child(&fix->scopes, fix->root, RC_STR("routine"));
    uint32_t anon    = scopes_make_child(&fix->scopes, fix->root, RC_STR("@0:12"));
    scopes_set_symbol(&fix->scopes, fix->root, RC_STR("top"),   value_make_numeric(1.0),  (cursor){0, 1});
    scopes_set_symbol(&fix->scopes, routine,   RC_STR("core"),  value_make_numeric(2.0),  (cursor){0, 2});
    scopes_set_symbol(&fix->scopes, anon,      RC_STR("hidden"),value_make_numeric(3.0),  (cursor){0, 3});
    scopes_set_symbol(&fix->scopes, fix->root, RC_STR("@0:20"), value_make_numeric(4.0),  (cursor){0, 4});   // a local label

    rc_arena scratch = rc_arena_make_default();
    rc_view_symbol_entry out = scopes_view_flatten(scopes_view_make(&fix->scopes), &fix->arena, scratch);

    // Spellable bindings appear, by their full path...
    RC_CHECK_TRUE(value_is_equal(flattened_lookup(out, RC_STR("top")),          value_make_numeric(1.0)));
    RC_CHECK_TRUE(value_is_equal(flattened_lookup(out, RC_STR("routine.core")), value_make_numeric(2.0)));
    // ...and the unspellable ones (anonymous-scope symbol, top-level local label) do not.
    RC_CHECK_TRUE(value_is_none(flattened_lookup(out, RC_STR("hidden"))));
    RC_CHECK_TRUE(value_is_none(flattened_lookup(out, RC_STR("@0:20"))));
    RC_CHECK(out.num, ==, 2u);   // exactly the two spellable symbols

    rc_arena_deinit(&scratch);
}

RC_TEST_STEP(scopes, view_get_symbol_from_root, fix)
{
    // scopes_view_get_symbol resolves a full path from the top level (root), off a projected read-only view:
    // a bare top-level name, a dotted path into a named child, and a miss all behave like scopes_get_symbol
    // from the root - proving the view carries everything a lookup needs (nodes + both pools).
    uint32_t routine = scopes_make_child(&fix->scopes, fix->root, RC_STR("routine"));
    scopes_set_symbol(&fix->scopes, fix->root, RC_STR("top"),  value_make_numeric(10.0), (cursor){0, 1});
    scopes_set_symbol(&fix->scopes, routine,   RC_STR("core"), value_make_numeric(20.0), (cursor){0, 2});

    scopes_view v = scopes_view_make(&fix->scopes);
    RC_CHECK_TRUE(value_is_equal(scopes_view_get_symbol(v, RC_STR("top")),          value_make_numeric(10.0)));
    RC_CHECK_TRUE(value_is_equal(scopes_view_get_symbol(v, RC_STR("routine.core")), value_make_numeric(20.0)));
    RC_CHECK_TRUE(value_is_none(scopes_view_get_symbol(v, RC_STR("routine.missing"))));
    RC_CHECK_TRUE(value_is_none(scopes_view_get_symbol(v, RC_STR("nope"))));
}

#endif // BARON_TESTS
