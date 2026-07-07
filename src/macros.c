#include "macros.h"

#include "richc/macros.h"


// Generous starting capacities so the shared arena rarely reallocates mid-pass (interleaved growth of
// several arrays in one arena is correct but copies, so we lean on reserve to avoid it).
enum {
    macros_list_reserve      = 16,
    macro_literals_reserve   = 4,
    macro_signatures_reserve = 2,
};

void macros_init(macros *m)
{
    RC_ASSERT(m != NULL);
    m->arena           = rc_arena_make_default();
    m->list            = rc_array_macro_make(macros_list_reserve, &m->arena);
    m->statement_tokens = (rc_array_token) {0};   // macros_reset seeds it from the base each pass
}

void macros_deinit(macros *m)
{
    RC_ASSERT(m != NULL);
    rc_arena_deinit(&m->arena);   // frees the list, the sub-arrays AND statement_tokens (all in this arena)
}

void macros_reset(macros *m, token_table base, uint32_t reserve_extra)
{
    RC_ASSERT(m != NULL);
    rc_arena_reset(&m->arena);   // reclaim everything - the list, the sub-arrays, the promoted slot views
    m->list             = rc_array_macro_make(macros_list_reserve, &m->arena);
    m->statement_tokens = rc_array_token_make_copy(base, base.num + reserve_extra, &m->arena);
}

token_table macros_statement_tokens(const macros *m)
{
    RC_ASSERT(m != NULL);
    return m->statement_tokens.view;
}

uint32_t macros_index_for_name(macros *m, rc_str name)
{
    RC_ASSERT(m != NULL);
    uint32_t t = token_table_find(m->statement_tokens.view, name);
    if (t != RC_INDEX_NONE) {
        token tok = rc_view_token_get(m->statement_tokens.view, t);
        if (tok.lexeme.type == lexeme_type_macro && tok.name.len == name.len) {
            return tok.lexeme.macro.index;
        }
    }
    uint32_t index = macros_add(m);
    rc_array_token_push(
        &m->statement_tokens,
        (token) {.name = name, .lexeme = {.type = lexeme_type_macro, .macro = {.index = index}}},
        &m->arena);
    return index;
}

uint32_t macros_add(macros *m)
{
    RC_ASSERT(m != NULL);
    macro entry = {
        .literal_table = rc_array_token_make(macro_literals_reserve, &m->arena),
        .signatures    = rc_array_macro_signature_make(macro_signatures_reserve, &m->arena),
    };
    return rc_array_macro_push(&m->list, entry, &m->arena);
}

macro *macros_at(macros *m, uint32_t index)
{
    RC_ASSERT(m != NULL);
    return rc_array_macro_at(&m->list, index);
}

uint32_t macros_intern_literal(macros *m, uint32_t index, rc_str text)
{
    macro *e = macros_at(m, index);
    for (uint32_t i = 0; i < e->literal_table.num; i++) {
        if (rc_str_is_equal_insensitive(rc_view_token_get(e->literal_table.view, i).name, text)) {
            return i;
        }
    }
    uint32_t id = e->literal_table.num;
    rc_array_token_push(
        &e->literal_table,
        (token) {.name = text, .lexeme = {.type = lexeme_type_macro_literal, .macro_literal = {.id = id}}},
        &m->arena);
    return id;
}

// Two signatures share a pattern when they have the same slots in the same order - same kinds, and for
// literals the same id (parameter NAMES are irrelevant: you cannot overload a macro by renaming its
// parameters, only by arity or intervening tokens). This is the overload identity.
static bool macro_pattern_equal(rc_view_macro_slot a, rc_view_macro_slot b)
{
    if (a.num != b.num) {
        return false;
    }
    for (uint32_t i = 0; i < a.num; i++) {
        macro_slot sa = rc_view_macro_slot_get(a, i);
        macro_slot sb = rc_view_macro_slot_get(b, i);
        if (sa.type != sb.type) {
            return false;
        }
        if (sa.type == macro_slot_literal && sa.literal_id != sb.literal_id) {
            return false;
        }
    }
    return true;
}

// The overload order: "match tokens before expressions". At the first slot two signatures differ, the one
// with a LITERAL there sorts first (it is the more specific at that position); if one is a strict prefix of
// the other, the longer (more constrained) sorts first; an identical pattern keeps definition order (stable).
// The matcher tries signatures in this order and takes the first full match.
static bool macro_signature_before(rc_view_macro_slot a, rc_view_macro_slot b)
{
    uint32_t n = a.num < b.num ? a.num : b.num;
    for (uint32_t i = 0; i < n; i++) {
        macro_slot sa = rc_view_macro_slot_get(a, i);
        macro_slot sb = rc_view_macro_slot_get(b, i);
        if (sa.type != sb.type) {
            // A fixed token (a literal or a comma) is more specific than a parameter, so it sorts first. If
            // neither is a parameter (a literal against a comma), leave them to length / definition order.
            if (sa.type == macro_slot_param) return false;
            if (sb.type == macro_slot_param) return true;
            return false;
        }
    }
    if (a.num != b.num) {
        return a.num > b.num;   // one is a prefix of the other: the longer, more constrained one first
    }
    return false;   // identical pattern: leave definition order to the stable insert
}

macro_add_status macros_add_signature(macros *m, uint32_t index, rc_view_macro_slot slots, cursor body, bool defined)
{
    macro *e = macros_at(m, index);

    // Reconcile against an existing overload of the same pattern (forward declaration -> fill; a second
    // real body -> duplicate; an empty body over anything -> a harmless no-op).
    for (uint32_t i = 0; i < e->signatures.num; i++) {
        macro_signature *sig = rc_array_macro_signature_at(&e->signatures, i);
        if (macro_pattern_equal(sig->slots, slots)) {
            if (!defined) {
                return macro_add_redundant;   // a repeated / superfluous forward declaration
            }
            if (!sig->defined) {
                sig->body    = body;          // supply the body a forward declaration was waiting for
                sig->defined = true;
                return macro_add_filled;
            }
            return macro_add_duplicate;       // two real bodies for one pattern
        }
    }

    // A new overload: insert it in specificity order (stable, so equal-rank keeps definition order).
    macro_signature ns = {.slots = slots, .body = body, .defined = defined};
    uint32_t pos = 0;
    while (pos < e->signatures.num
           && !macro_signature_before(ns.slots, rc_array_macro_signature_get(&e->signatures, pos).slots)) {
        pos++;
    }
    rc_array_macro_signature_insert(&e->signatures, pos, ns, &m->arena);
    return macro_add_inserted;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Build a param slot and a literal slot for tests (literal_id set directly, no interning needed here).
static macro_slot t_param(void)              { return (macro_slot) {.type = macro_slot_param}; }
static macro_slot t_literal(uint32_t id)     { return (macro_slot) {.type = macro_slot_literal, .literal_id = id}; }

// A signature view over a caller-owned slot array.
static rc_view_macro_slot t_sig(const macro_slot *slots, uint32_t n)
{
    return (rc_view_macro_slot) {.data = slots, .num = n};
}

RC_TEST(macros, ordering_literal_before_param)
{
    // LAX "(" addr ")"  vs  LAX addr : the parenthesised one (a literal at slot 0) sorts first.
    macro_slot paren[] = {t_literal(0), t_param(), t_literal(1)};
    macro_slot bare[]  = {t_param()};

    macros m;
    macros_init(&m);
    uint32_t lax = macros_add(&m);

    // Register the bare one FIRST, so a correct sort must reorder it after the literal-led one.
    RC_CHECK_TRUE(macros_add_signature(&m, lax, t_sig(bare, 1),  (cursor){0, 10}, true) == macro_add_inserted);
    RC_CHECK_TRUE(macros_add_signature(&m, lax, t_sig(paren, 3), (cursor){0, 20}, true) == macro_add_inserted);

    macro *e = macros_at(&m, lax);
    RC_CHECK(e->signatures.num, ==, 2u);
    // Slot 0 of the first signature is a literal (the paren form won the sort).
    RC_CHECK_TRUE(rc_view_macro_slot_get(rc_array_macro_signature_get(&e->signatures, 0).slots, 0).type == macro_slot_literal);
    RC_CHECK_TRUE(rc_view_macro_slot_get(rc_array_macro_signature_get(&e->signatures, 1).slots, 0).type == macro_slot_param);

    macros_deinit(&m);
}

RC_TEST(macros, forward_declaration_fill_and_duplicate)
{
    macro_slot one[] = {t_param()};

    macros m;
    macros_init(&m);
    uint32_t a = macros_add(&m);

    // A forward declaration (empty body) then its fill-in is not a duplicate.
    RC_CHECK_TRUE(macros_add_signature(&m, a, t_sig(one, 1), (cursor){0, 0},  false) == macro_add_inserted);
    RC_CHECK_TRUE(macros_add_signature(&m, a, t_sig(one, 1), (cursor){0, 40}, true)  == macro_add_filled);
    RC_CHECK(macros_at(&m, a)->signatures.num, ==, 1u);
    RC_CHECK_TRUE(rc_array_macro_signature_get(&macros_at(&m, a)->signatures, 0).defined);

    // A second real body for the same pattern IS a duplicate (and does not change the stored body).
    RC_CHECK_TRUE(macros_add_signature(&m, a, t_sig(one, 1), (cursor){0, 80}, true) == macro_add_duplicate);
    RC_CHECK(rc_array_macro_signature_get(&macros_at(&m, a)->signatures, 0).body.pos, ==, 40u);

    macros_deinit(&m);
}

RC_TEST(macros, intern_literal_dedupes)
{
    macros m;
    macros_init(&m);
    uint32_t a = macros_add(&m);

    uint32_t hash1 = macros_intern_literal(&m, a, RC_STR("#"));
    uint32_t comma = macros_intern_literal(&m, a, RC_STR(","));
    uint32_t hash2 = macros_intern_literal(&m, a, RC_STR("#"));   // same text -> same id
    RC_CHECK(hash1, ==, hash2);
    RC_CHECK_TRUE(hash1 != comma);
    RC_CHECK(macros_at(&m, a)->literal_table.num, ==, 2u);

    macros_deinit(&m);
}

RC_TEST(macros, reset_empties)
{
    macros m;
    macros_init(&m);
    macros_add(&m);
    macros_add(&m);
    RC_CHECK(m.list.num, ==, 2u);
    macros_reset(&m, (token_table) {0}, 0);   // no base table needed for this store-only check
    RC_CHECK(m.list.num, ==, 0u);
    // The store is usable again after a reset.
    RC_CHECK(macros_add(&m), ==, 0u);
    macros_deinit(&m);
}

#endif // BARON_TESTS
