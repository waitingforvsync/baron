#include "scopes.h"

#include "richc/macros.h"
#include "richc/mstr.h"


// A symbol or scope name is a single identifier: it must carry no path separator.
static bool is_leaf_name(rc_str name)
{
    return rc_str_find_first(name, RC_STR(".")) == RC_INDEX_NONE;
}


void scopes_init(scopes *s)
{
    RC_ASSERT(s != NULL);
    s->node_arena   = rc_arena_make_default();
    s->symbol_arena = rc_arena_make_default();
    s->child_arena  = rc_arena_make_default();
    s->value_arena  = rc_arena_make_default();
    s->nodes        = rc_array_scope_node_make(0, &s->node_arena);
    s->symbol_pool  = rc_trie_symbol_pool_make(0, &s->symbol_arena);
    s->child_pool   = rc_trie_child_pool_make(0, &s->child_arena);
}

void scopes_deinit(scopes *s)
{
    RC_ASSERT(s != NULL);
    rc_arena_deinit(&s->node_arena);
    rc_arena_deinit(&s->symbol_arena);
    rc_arena_deinit(&s->child_arena);
    rc_arena_deinit(&s->value_arena);
}

void scopes_reset(scopes *s)
{
    RC_ASSERT(s != NULL);
    // The tries squirrel pointers back into the pools, so we cannot cheaply truncate: tear the whole
    // thing down and stand a fresh, empty tree back up (a bare root at index 0). Only runs on failure.
    scopes_deinit(s);
    scopes_init(s);
    scopes_make_root(s);
}

uint32_t scopes_make_root(scopes *s)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(rc_array_scope_node_is_empty(&s->nodes));   // the root is the first node

    return rc_array_scope_node_push(
        &s->nodes,
        (scope_node) {
            .name     = (rc_str) {0},
            .parent   = RC_INDEX_NONE,
            .symbols  = rc_trie_symbol_make(&s->symbol_pool, &s->symbol_arena),
            .children = rc_trie_child_make(&s->child_pool, &s->child_arena),
        },
        &s->node_arena);
}

uint32_t scopes_make_child(scopes *s, uint32_t parent_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    // A real (source) child name is a single identifier; a synthetic anonymous-scope key the caller
    // builds ('@source:pos...') deliberately is not, so we only insist the bytes outlive the scopes.

    uint32_t child_index = rc_array_scope_node_push(
        &s->nodes,
        (scope_node) {
            .name     = name,
            .parent   = parent_index,
            .symbols  = rc_trie_symbol_make(&s->symbol_pool, &s->symbol_arena),
            .children = rc_trie_child_make(&s->child_pool, &s->child_arena),
        },
        &s->node_arena);

    // Reach the parent fresh after the push: it may have shuffled the nodes array
    // along. An anonymous child is reachable only by index, so we leave it off the
    // child map.
    if (name.len > 0) {
        rc_trie_child_add(
            &RC_AT(s->nodes, parent_index).children,
            name,
            child_index,
            &s->child_arena);
    }
    return child_index;
}

uint32_t scopes_get_or_make_child(scopes *s, uint32_t parent_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(name.len > 0);

    rc_trie_child *kids  = &RC_AT(s->nodes, parent_index).children;
    uint32_t       found = rc_trie_child_find(kids, name);   // probe with the caller's (maybe scratch) view
    if (found != RC_INDEX_NONE) {
        return rc_trie_child_value_get(kids, found);
    }

    // First sighting: own the key so the caller may hand us a scratch buffer. It goes in value_arena
    // (not child_arena) to leave the child_pool the sole tenant of its arena, free to grow in place.
    rc_str owned = rc_mstr_from_str(name, name.len, &s->value_arena).view;
    return scopes_make_child(s, parent_index, owned);
}

symbol_status scopes_set_symbol(scopes *s, uint32_t scope_index, rc_str name, value v, cursor def)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(is_leaf_name(name));

    rc_trie_symbol *syms  = &RC_AT(s->nodes, scope_index).symbols;
    uint32_t        found = rc_trie_symbol_find(syms, name);
    if (found != RC_INDEX_NONE) {
        symbol existing = rc_trie_symbol_value_get(syms, found);
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
        rc_trie_symbol_value_set(syms, found, (symbol){ value_make_copy(v, &s->value_arena), def });
        return symbol_status_changed;
    }
    // First binding: own the key in value_arena too, so the caller may hand us a scratch view - a local
    // label's synthetic '@source:pos' key has no backing in the source text. Only the add path copies (a
    // later pass finds the key by content and reuses it), so this is one copy per symbol, not per pass.
    rc_str owned = rc_mstr_from_str(name, name.len, &s->value_arena).view;
    rc_trie_symbol_add(syms, owned, (symbol){ value_make_copy(v, &s->value_arena), def }, &s->symbol_arena);
    return symbol_status_unchanged;   // brand new, so there was nothing to converge from
}

bool scopes_remove_symbol(scopes *s, uint32_t scope_index, rc_str name)
{
    RC_ASSERT(s != NULL);
    RC_ASSERT(is_leaf_name(name));
    return rc_trie_symbol_delete(&RC_AT(s->nodes, scope_index).symbols, name);
}

cursor scopes_symbol_def(const scopes *s, uint32_t scope_index, rc_str name)
{
    RC_ASSERT(s != NULL && is_leaf_name(name));

    rc_trie_symbol *syms  = &RC_AT(s->nodes, scope_index).symbols;
    uint32_t        found = rc_trie_symbol_find(syms, name);
    return found == RC_INDEX_NONE
               ? cursor_none()
               : rc_trie_symbol_value_get(syms, found).def;
}

value scopes_get_symbol(const scopes *s, uint32_t scope_index, rc_str full_path)
{
    RC_ASSERT(s != NULL);

    rc_str      dot   = RC_STR(".");
    rc_str_pair split = rc_str_first_split(full_path, dot);

    // Bare name: walk up the parent chain, and the nearest enclosing definition wins.
    if (!rc_str_is_valid(split.second)) {
        for (uint32_t i = scope_index; i != RC_INDEX_NONE; i = RC_AT(s->nodes, i).parent) {
            rc_trie_symbol *syms  = &RC_AT(s->nodes, i).symbols;
            uint32_t        found = rc_trie_symbol_find(syms, full_path);
            if (found != RC_INDEX_NONE) {
                return rc_trie_symbol_value_get(syms, found).v;
            }
        }
        return value_make_none();
    }

    // Dotted path: find the head the same way (walk up the parents), then descend
    // the rest strictly through the child maps.
    uint32_t cur  = RC_INDEX_NONE;
    rc_str   head = split.first;
    for (uint32_t i = scope_index; i != RC_INDEX_NONE; i = RC_AT(s->nodes, i).parent) {
        rc_trie_child *kids  = &RC_AT(s->nodes, i).children;
        uint32_t       found = rc_trie_child_find(kids, head);
        if (found != RC_INDEX_NONE) {
            cur = rc_trie_child_value_get(kids, found);
            break;
        }
    }
    if (cur == RC_INDEX_NONE) {
        return value_make_none();
    }

    rc_str rest = split.second;
    while (true) {
        split = rc_str_first_split(rest, dot);
        if (!rc_str_is_valid(split.second)) {
            // Last component: the symbol name, looked up in the leaf scope alone.
            rc_trie_symbol *syms  = &RC_AT(s->nodes, cur).symbols;
            uint32_t        found = rc_trie_symbol_find(syms, rest);
            return found == RC_INDEX_NONE ? value_make_none() : rc_trie_symbol_value_get(syms, found).v;
        }
        // Intermediate component: step down into the named child scope.
        rc_trie_child *kids  = &RC_AT(s->nodes, cur).children;
        uint32_t       found = rc_trie_child_find(kids, split.first);
        if (found == RC_INDEX_NONE) {
            return value_make_none();
        }
        cur  = rc_trie_child_value_get(kids, found);
        rest = split.second;
    }
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
static void local_label_visit(local_label_search *search, rc_trie_symbol *t, uint32_t index)
{
    rc_str key = rc_trie_symbol_key_get(t, index);
    if (!rc_str_starts_with(key, RC_STR("@"))) {
        return;   // an ordinary named symbol, not a local label
    }
    symbol sym = rc_trie_symbol_value_get(t, index);
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

#define RC_TRIE_FOREACH_TYPE          symbol
#define RC_TRIE_FOREACH_CTX           local_label_search
#define RC_TRIE_FOREACH_FUNC(c, t, i) local_label_visit(c, t, i)
#include "richc/template/algorithm/hash_trie_foreach.h"   // rc_trie_foreach_symbol

value scopes_find_local_label(const scopes *s, uint32_t scope_index, uint32_t source, uint32_t use_pos, bool forward)
{
    RC_ASSERT(s != NULL);
    local_label_search search = {
        .source  = source,
        .use_pos = use_pos,
        .forward = forward,
    };
    rc_trie_foreach_symbol(&RC_AT(s->nodes, scope_index).symbols, &search);
    // No candidate: report it as an unknown symbol, so an unresolved @- / @+ defers like any forward
    // reference (and becomes undefined_symbol on the final pass if it never binds).
    return search.found ? search.result : value_make_error(error_type_unknown_symbol);
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Each test gets a fresh scope tree with its root already made.
RC_TEST_GROUP_DATA(scopes) {
    scopes   scopes;
    uint32_t root;
};

RC_TEST_GROUP_INIT(scopes, fix)
{
    scopes_init(&fix->scopes);
    fix->root = scopes_make_root(&fix->scopes);
}

RC_TEST_GROUP_DEINIT(scopes, fix)
{
    scopes_deinit(&fix->scopes);
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

#endif // BARON_TESTS
