#include "zpalloc.h"

#include "richc/macros.h"


enum { zp_bytes = 256 };   // the zero page is one 256-byte page; a base + width must fit inside it


// Do vregs a and b conflict for colouring? They conflict iff their live ranges overlap (they interfere).
// Unused variables never reach this test: they are skipped by the placement loop (no address at all -
// the finalizer warns and undefines them), so a placed variable is always a used one.
static bool conflicts(const liveness *lv, uint32_t a, uint32_t b)
{
    return liveness_interferes(lv, a, b);
}

// Are all `width` bytes starting at `base` reserved (and inside the page)?
static bool span_reserved(const rc_bitset *reserved, uint32_t base, uint32_t width)
{
    if (base + width > zp_bytes) {
        return false;
    }
    for (uint32_t i = 0; i < width; i++) {
        if (!rc_bitset_is_set(reserved, base + i)) {
            return false;
        }
    }
    return true;
}

// Do byte spans [a, a+aw) and [b, b+bw) overlap?
static bool spans_overlap(uint32_t a, uint32_t aw, uint32_t b, uint32_t bw)
{
    return a < b + bw && b < a + aw;
}

zp_coloring zp_color(const liveness *lv, rc_view_zp_var vars, const rc_bitset *reserved,
                     rc_arena *arena, rc_arena scratch)
{
    (void) scratch;
    uint32_t n = vars.num;
    zp_coloring col = {
        .num_vars    = n,
        .base        = n ? rc_arena_alloc_type(arena, uint32_t, n) : NULL,
        .any_spilled = false,
    };
    for (uint32_t v = 0; v < n; v++) {
        col.base[v] = RC_INDEX_NONE;
    }

    // First-fit-decreasing over widths (widest first): place the more constrained wide variables before the
    // narrow ones, each at the lowest reserved base that clashes with no already-placed conflicting variable.
    // Left-edge-optimal for the equal-width common case; good enough for a mix of widths (1, 2, or a ZA_AUTO <n>
    // table). The width set is unknown, so we sweep every width from the widest present down to 1.
    uint32_t max_w = 1;
    for (uint32_t v = 0; v < n; v++) {
        uint32_t w = rc_view_zp_var_get(vars, v).width;
        if (w > max_w) {
            max_w = w;
        }
    }
    for (uint32_t pass_w = max_w; pass_w >= 1; pass_w--) {
        for (uint32_t v = 0; v < n; v++) {
            uint32_t w = rc_view_zp_var_get(vars, v).width;
            if (w != pass_w) {
                continue;
            }
            if (liveness_class_of(lv, v) == vreg_class_unused) {
                continue;   // no instruction touches it: no address (base stays NONE, and NOT a spill)
            }
            for (uint32_t base = 0; base + w <= zp_bytes; base++) {
                if (!span_reserved(reserved, base, w)) {
                    continue;
                }
                bool clash = false;
                for (uint32_t u = 0; u < n; u++) {
                    if (u == v || col.base[u] == RC_INDEX_NONE || !conflicts(lv, v, u)) {
                        continue;
                    }
                    uint32_t uw = rc_view_zp_var_get(vars, u).width;
                    if (spans_overlap(base, w, col.base[u], uw)) {
                        clash = true;
                        break;
                    }
                }
                if (!clash) {
                    col.base[v] = base;
                    break;
                }
            }
            if (col.base[v] == RC_INDEX_NONE) {
                col.any_spilled = true;
            }
        }
    }
    return col;
}


#ifdef BARON_TESTS

#include "richc/test.h"
#include "cfg.h"

// Build a reserved-byte set over [lo, hi] for the tests.
static rc_bitset reserve_range(uint32_t lo, uint32_t hi, rc_arena *arena)
{
    rc_bitset bs = {0};
    rc_bitset_resize(&bs, zp_bytes, arena);
    for (uint32_t b = lo; b <= hi; b++) {
        rc_bitset_set(&bs, b);
    }
    return bs;
}

// One var of the given width; def cursor is unique per index so the registry stays well-formed.
static void add_var(rc_array_zp_var *vars, uint8_t width, rc_arena *arena)
{
    uint32_t i = vars->num;
    rc_array_zp_var_push(vars,
        (zp_var) {.name = RC_STR("v"), .scope = 0, .width = width, .def = (cursor) {.source = 0, .pos = i}},
        arena);
}

RC_TEST(zpalloc, disjoint_share_interfering_split)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();

    // Three width-1 vars; the test wires the interference directly by handing zp_color a liveness whose graph
    // we build by hand: v0 and v1 interfere, v2 is disjoint from both. All three are "used" so they place
    // (an unused var is skipped entirely - covered below).
    rc_array_zp_var vars = rc_array_zp_var_make(4, &arena);
    add_var(&vars, 1, &arena); add_var(&vars, 1, &arena); add_var(&vars, 1, &arena);

    // Hand-built liveness: 3 vars, interfere[0]<->[1]; classes all input (used). num_blocks 0 is fine here.
    liveness lv = {.num_vars = 3, .num_blocks = 0};
    lv.interfere = rc_arena_alloc_type(&arena, rc_bitset, 3);
    for (uint32_t i = 0; i < 3; i++) { lv.interfere[i] = (rc_bitset) {0}; rc_bitset_resize(&lv.interfere[i], 3, &arena); }
    rc_bitset_set(&lv.interfere[0], 1); rc_bitset_set(&lv.interfere[1], 0);
    lv.classes = rc_arena_alloc_type(&arena, vreg_class, 3);
    lv.classes[0] = lv.classes[1] = lv.classes[2] = vreg_class_input;

    rc_bitset reserved = reserve_range(0x70, 0x7F, &arena);
    zp_coloring col = zp_color(&lv, vars.view, &reserved, &arena, scratch);

    RC_CHECK_FALSE(col.any_spilled);
    RC_CHECK(col.base[0], ==, 0x70u);   // first placed
    RC_CHECK(col.base[1], ==, 0x71u);   // interferes with v0 -> next byte
    RC_CHECK(col.base[2], ==, 0x70u);   // disjoint from both -> reuses v0's byte

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(zpalloc, width_two_takes_consecutive_bytes)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();

    // A 2-byte pointer and a 1-byte temp that interfere: the pointer (placed first, FFD) takes 0x70-0x71, the
    // temp cannot overlap it, so it lands at 0x72.
    rc_array_zp_var vars = rc_array_zp_var_make(4, &arena);
    add_var(&vars, 2, &arena);   // v0 pointer
    add_var(&vars, 1, &arena);   // v1 temp

    liveness lv = {.num_vars = 2, .num_blocks = 0};
    lv.interfere = rc_arena_alloc_type(&arena, rc_bitset, 2);
    for (uint32_t i = 0; i < 2; i++) { lv.interfere[i] = (rc_bitset) {0}; rc_bitset_resize(&lv.interfere[i], 2, &arena); }
    rc_bitset_set(&lv.interfere[0], 1); rc_bitset_set(&lv.interfere[1], 0);
    lv.classes = rc_arena_alloc_type(&arena, vreg_class, 2);
    lv.classes[0] = vreg_class_temp; lv.classes[1] = vreg_class_temp;

    rc_bitset reserved = reserve_range(0x70, 0x7F, &arena);
    zp_coloring col = zp_color(&lv, vars.view, &reserved, &arena, scratch);

    RC_CHECK_FALSE(col.any_spilled);
    RC_CHECK(col.base[0], ==, 0x70u);   // 2-byte pointer, first by FFD, at 0x70-0x71
    RC_CHECK(col.base[1], ==, 0x72u);   // 1-byte temp cannot overlap -> 0x72

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(zpalloc, unused_variable_is_skipped)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();

    // An unused variable gets NO address - not a byte of its own, and not a spill either - so the pool is
    // spent only on variables the instruction stream actually touches. v1 (unused) is skipped; v0 and v2
    // interfere and pack as if it were never declared.
    rc_array_zp_var vars = rc_array_zp_var_make(4, &arena);
    add_var(&vars, 1, &arena); add_var(&vars, 1, &arena); add_var(&vars, 1, &arena);

    liveness lv = {.num_vars = 3, .num_blocks = 0};
    lv.interfere = rc_arena_alloc_type(&arena, rc_bitset, 3);
    for (uint32_t i = 0; i < 3; i++) { lv.interfere[i] = (rc_bitset) {0}; rc_bitset_resize(&lv.interfere[i], 3, &arena); }
    rc_bitset_set(&lv.interfere[0], 2); rc_bitset_set(&lv.interfere[2], 0);
    lv.classes = rc_arena_alloc_type(&arena, vreg_class, 3);
    lv.classes[0] = vreg_class_temp; lv.classes[1] = vreg_class_unused; lv.classes[2] = vreg_class_temp;

    rc_bitset reserved = reserve_range(0x70, 0x7F, &arena);
    zp_coloring col = zp_color(&lv, vars.view, &reserved, &arena, scratch);

    RC_CHECK_FALSE(col.any_spilled);            // a skipped unused var is NOT a spill
    RC_CHECK(col.base[0], ==, 0x70u);
    RC_CHECK(col.base[1], ==, RC_INDEX_NONE);   // no address at all
    RC_CHECK(col.base[2], ==, 0x71u);           // packs as if v1 were never declared

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(zpalloc, spill_when_out_of_bytes)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();

    // Two mutually-interfering vars but only ONE reserved byte -> the second cannot be placed (spill).
    rc_array_zp_var vars = rc_array_zp_var_make(4, &arena);
    add_var(&vars, 1, &arena); add_var(&vars, 1, &arena);

    liveness lv = {.num_vars = 2, .num_blocks = 0};
    lv.interfere = rc_arena_alloc_type(&arena, rc_bitset, 2);
    for (uint32_t i = 0; i < 2; i++) { lv.interfere[i] = (rc_bitset) {0}; rc_bitset_resize(&lv.interfere[i], 2, &arena); }
    rc_bitset_set(&lv.interfere[0], 1); rc_bitset_set(&lv.interfere[1], 0);
    lv.classes = rc_arena_alloc_type(&arena, vreg_class, 2);
    lv.classes[0] = vreg_class_temp; lv.classes[1] = vreg_class_temp;

    rc_bitset reserved = reserve_range(0x70, 0x70, &arena);   // exactly one byte
    zp_coloring col = zp_color(&lv, vars.view, &reserved, &arena, scratch);

    RC_CHECK_TRUE(col.any_spilled);
    RC_CHECK(col.base[0], ==, 0x70u);
    RC_CHECK(col.base[1], ==, RC_INDEX_NONE);   // spilled

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
