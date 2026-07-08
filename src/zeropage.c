#include "zeropage.h"

#include "richc/macros.h"


enum {
    zeropage_vars_reserve  = 64,    // a very large routine's worth of locals; grows if exceeded
    zeropage_insns_reserve = 256,   // a comfortable straight-line routine's worth of VAR touches
};

void zeropage_init(zeropage *zp, rc_arena *permanent)
{
    RC_ASSERT(zp != NULL && permanent != NULL);
    zp->arena    = permanent;
    zp->reserved = (rc_bitset) {0};
    rc_bitset_resize(&zp->reserved, zeropage_size, permanent);   // 256 addressable, all zero
    zp->vars     = rc_array_zp_var_make(zeropage_vars_reserve, permanent);
    zp->insns    = rc_array_zp_insn_make(zeropage_insns_reserve, permanent);
    zp->enabled  = false;
}

void zeropage_reset(zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    rc_bitset_reset(&zp->reserved);          // clears every bit, keeps num/cap
    rc_array_zp_var_resize(&zp->vars, 0, zp->arena);     // clears the lists, keeps the backing
    rc_array_zp_insn_resize(&zp->insns, 0, zp->arena);
    zp->enabled = false;
}

void zeropage_enable(zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    zp->enabled = true;
}

void zeropage_reserve(zeropage *zp, uint32_t byte)
{
    RC_ASSERT(zp != NULL && byte < zeropage_size);
    rc_bitset_set(&zp->reserved, byte);
    zp->enabled = true;   // reserving a byte implies the feature is on, even before an explicit enable
}

bool zeropage_is_enabled(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return zp->enabled;
}

bool zeropage_is_reserved(const zeropage *zp, uint32_t byte)
{
    RC_ASSERT(zp != NULL);
    return byte < zeropage_size && rc_bitset_is_set(&zp->reserved, byte);
}

uint32_t zeropage_reserved_count(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    uint32_t count = 0;
    for (uint32_t b = rc_bitset_get_first_set(&zp->reserved);
         b != RC_INDEX_NONE;
         b = rc_bitset_get_next_set(&zp->reserved, b + 1)) {
        count++;
    }
    return count;
}

uint32_t zeropage_add_var(zeropage *zp, rc_str name, uint32_t scope, uint8_t width, cursor def)
{
    RC_ASSERT(zp != NULL);
    return rc_array_zp_var_push(
        &zp->vars,
        (zp_var) {.name = name, .scope = scope, .width = width, .def = def},
        zp->arena);
}

uint32_t zeropage_var_count(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return zp->vars.num;
}

zp_var zeropage_var_get(const zeropage *zp, uint32_t index)
{
    RC_ASSERT(zp != NULL);
    return rc_array_zp_var_get(&zp->vars, index);
}

rc_view_zp_var zeropage_vars(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return zp->vars.view;
}

const rc_bitset *zeropage_reserved(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return &zp->reserved;
}

uint32_t zeropage_find_var(const zeropage *zp, uint32_t scope, cursor def)
{
    RC_ASSERT(zp != NULL);
    for (uint32_t i = 0; i < zp->vars.num; i++) {
        zp_var v = rc_array_zp_var_get(&zp->vars, i);
        if (v.scope == scope && cursor_is_equal(v.def, def)) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

uint32_t zeropage_add_insn(zeropage *zp, zp_insn insn)
{
    RC_ASSERT(zp != NULL);
    return rc_array_zp_insn_push(&zp->insns, insn, zp->arena);
}

void zeropage_resolve_vregs(zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    for (uint32_t i = 0; i < zp->insns.num; i++) {
        zp_insn *n = rc_array_zp_insn_at(&zp->insns, i);
        n->vreg = cursor_is_none(n->var_def) ? RC_INDEX_NONE
                                             : zeropage_find_var(zp, n->var_scope, n->var_def);
    }
}

uint32_t zeropage_insn_count(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return zp->insns.num;
}

zp_insn zeropage_insn_get(const zeropage *zp, uint32_t index)
{
    RC_ASSERT(zp != NULL);
    return rc_array_zp_insn_get(&zp->insns, index);
}

rc_view_zp_insn zeropage_insns(const zeropage *zp)
{
    RC_ASSERT(zp != NULL);
    return zp->insns.view;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(zeropage, reserve_and_query)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    // A fresh set is empty and dormant.
    RC_CHECK_FALSE(zeropage_is_enabled(&zp));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 0u);
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x70));

    // Reserving a byte switches the feature on and records exactly that byte.
    zeropage_reserve(&zp, 0x70);
    RC_CHECK_TRUE(zeropage_is_enabled(&zp));
    RC_CHECK_TRUE(zeropage_is_reserved(&zp, 0x70));
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x71));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 1u);

    // Idempotent: re-reserving the same byte does not double-count.
    zeropage_reserve(&zp, 0x70);
    RC_CHECK(zeropage_reserved_count(&zp), ==, 1u);

    // A span of bytes, and the boundary byte 0xFF.
    for (uint32_t b = 0x80; b <= 0x8F; b++) {
        zeropage_reserve(&zp, b);
    }
    zeropage_reserve(&zp, 0xFF);
    RC_CHECK(zeropage_reserved_count(&zp), ==, 1u + 16u + 1u);
    RC_CHECK_TRUE(zeropage_is_reserved(&zp, 0x8F));
    RC_CHECK_TRUE(zeropage_is_reserved(&zp, 0xFF));

    // Out-of-page queries are simply false, never a trap.
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, zeropage_size));
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x1000));

    rc_arena_deinit(&arena);
}

RC_TEST(zeropage, enable_without_bytes)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    // ZPRESERVE with an empty list enables the feature but reserves nothing.
    zeropage_enable(&zp);
    RC_CHECK_TRUE(zeropage_is_enabled(&zp));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 0u);

    rc_arena_deinit(&arena);
}

RC_TEST(zeropage, var_registry)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    RC_CHECK(zeropage_var_count(&zp), ==, 0u);

    uint32_t i0 = zeropage_add_var(&zp, RC_STR("foo"), 3, 1, (cursor) {.source = 0, .pos = 10});
    uint32_t i1 = zeropage_add_var(&zp, RC_STR("ptr"), 3, 2, (cursor) {.source = 0, .pos = 20});
    RC_CHECK(i0, ==, 0u);
    RC_CHECK(i1, ==, 1u);
    RC_CHECK(zeropage_var_count(&zp), ==, 2u);

    zp_var v0 = zeropage_var_get(&zp, 0);
    RC_CHECK(v0.name, ==, RC_STR("foo"));
    RC_CHECK(v0.width, ==, (uint8_t) 1);
    RC_CHECK(v0.scope, ==, 3u);
    RC_CHECK(v0.def.pos, ==, 10u);

    zp_var v1 = zeropage_var_get(&zp, 1);
    RC_CHECK(v1.width, ==, (uint8_t) 2);

    // find_var maps a binding's identity - the (scope, def) PAIR - back to its vreg index (or NONE).
    RC_CHECK(zeropage_find_var(&zp, 3, (cursor) {.source = 0, .pos = 20}), ==, 1u);
    RC_CHECK(zeropage_find_var(&zp, 3, (cursor) {.source = 0, .pos = 10}), ==, 0u);
    RC_CHECK(zeropage_find_var(&zp, 3, (cursor) {.source = 0, .pos = 99}), ==, RC_INDEX_NONE);
    // Same def cursor, different scope -> a different variable (this is what lets macro instances differ).
    RC_CHECK(zeropage_find_var(&zp, 5, (cursor) {.source = 0, .pos = 20}), ==, RC_INDEX_NONE);

    rc_arena_deinit(&arena);
}

RC_TEST(zeropage, insn_list)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    RC_CHECK(zeropage_insn_count(&zp), ==, 0u);
    zeropage_add_insn(&zp, (zp_insn) {.pc = 0x2000, .size = 2, .flow = zp_flow_normal,
                                      .vreg = 0, .rw = vref_write, .target = RC_INDEX_NONE,
                                      .at = (cursor) {.source = 0, .pos = 4}});
    zeropage_add_insn(&zp, (zp_insn) {.pc = 0x2002, .size = 2, .flow = zp_flow_normal,
                                      .vreg = 0, .rw = vref_read | vref_write, .target = RC_INDEX_NONE,
                                      .at = (cursor) {.source = 0, .pos = 8}});
    RC_CHECK(zeropage_insn_count(&zp), ==, 2u);
    RC_CHECK((int) zeropage_insn_get(&zp, 0).rw, ==, (int) vref_write);
    RC_CHECK((int) zeropage_insn_get(&zp, 1).rw, ==, (int) (vref_read | vref_write));
    RC_CHECK(zeropage_insn_get(&zp, 1).at.pos, ==, 8u);
    RC_CHECK(zeropage_insn_get(&zp, 1).pc, ==, 0x2002u);

    rc_arena_deinit(&arena);
}

RC_TEST(zeropage, reset_clears_for_next_pass)
{
    rc_arena arena = rc_arena_make_default();
    zeropage zp;
    zeropage_init(&zp, &arena);

    zeropage_reserve(&zp, 0x70);
    zeropage_reserve(&zp, 0x71);
    zeropage_add_var(&zp, RC_STR("foo"), 0, 1, (cursor) {0});
    zeropage_add_insn(&zp, (zp_insn) {.vreg = 0, .rw = vref_read, .target = RC_INDEX_NONE});
    RC_CHECK(zeropage_reserved_count(&zp), ==, 2u);
    RC_CHECK(zeropage_var_count(&zp), ==, 1u);
    RC_CHECK(zeropage_insn_count(&zp), ==, 1u);

    // A new pass starts from an empty, dormant set with no vars or insns (the backing is kept).
    zeropage_reset(&zp);
    RC_CHECK_FALSE(zeropage_is_enabled(&zp));
    RC_CHECK(zeropage_reserved_count(&zp), ==, 0u);
    RC_CHECK_FALSE(zeropage_is_reserved(&zp, 0x70));
    RC_CHECK(zeropage_var_count(&zp), ==, 0u);
    RC_CHECK(zeropage_insn_count(&zp), ==, 0u);

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
