#include "liveness.h"

#include "richc/macros.h"


// The small set algebra the fixpoint needs (copy / union / equality) lives in richc's rc_bitset now. Every
// bitset in a given analysis has the same width (num_vars), so the equal-width preconditions hold throughout.

// Allocate `n` zeroed bitsets each `width` bits wide (NULL if n == 0).
static rc_bitset *make_rows(uint32_t n, uint32_t width, rc_arena *arena)
{
    if (n == 0) {
        return NULL;
    }
    rc_bitset *rows = rc_arena_alloc_type(arena, rc_bitset, n);
    for (uint32_t i = 0; i < n; i++) {
        rows[i] = (rc_bitset) {0};
        rc_bitset_resize(&rows[i], width, arena);
    }
    return rows;
}

// The vreg an instruction reads (RC_INDEX_NONE if it reads none), and likewise for writes. A single
// instruction touches at most one vreg, in one or both directions (an RMW like INC is both).
static uint32_t insn_use(zp_insn n) { return (n.vreg != RC_INDEX_NONE && (n.rw & vref_read))  ? n.vreg : RC_INDEX_NONE; }
static uint32_t insn_def(zp_insn n) { return (n.vreg != RC_INDEX_NONE && (n.rw & vref_write)) ? n.vreg : RC_INDEX_NONE; }

static void add_edge(rc_bitset *interfere, uint32_t a, uint32_t b)
{
    if (a != b) {
        rc_bitset_set(&interfere[a], b);
        rc_bitset_set(&interfere[b], a);
    }
}

liveness liveness_analyze(cfg g, rc_view_zp_insn insns, uint32_t num_vars, uint32_t entry_block,
                          rc_arena *arena, rc_arena scratch)
{
    uint32_t nb = g.blocks.num;
    liveness lv = {
        .num_vars   = num_vars,
        .num_blocks = nb,
        .live_in    = make_rows(nb, num_vars, arena),
        .live_out   = make_rows(nb, num_vars, arena),
        .interfere  = make_rows(num_vars, num_vars, arena),
        .classes    = num_vars ? rc_arena_alloc_zero_type(arena, vreg_class, num_vars) : NULL,
    };
    if (nb == 0 || num_vars == 0) {
        return lv;
    }

    // Per-block use/def, computed once. use[b] = vars read before any write in the block; def[b] = vars
    // written anywhere in the block. Building def[b] forward doubles as the "seen a def yet" test for use[b].
    rc_bitset *use = make_rows(nb, num_vars, &scratch);
    rc_bitset *def = make_rows(nb, num_vars, &scratch);
    for (uint32_t b = 0; b < nb; b++) {
        basic_block block = rc_array_basic_block_get(&g.blocks, b);
        for (uint32_t k = 0; k < block.num_insns; k++) {
            zp_insn n = rc_view_zp_insn_get(insns, block.first_insn + k);
            uint32_t u = insn_use(n);
            uint32_t d = insn_def(n);
            if (u != RC_INDEX_NONE && !rc_bitset_is_set(&def[b], u)) {
                rc_bitset_set(&use[b], u);
            }
            if (d != RC_INDEX_NONE) {
                rc_bitset_set(&def[b], d);
            }
        }
    }

    // A full set for the unknown_succ taint: a block whose control may leave to an address we cannot model
    // must assume every var is live out (the computed target might read any of them).
    rc_bitset full = {0};
    rc_bitset_resize(&full, num_vars, &scratch);
    for (uint32_t v = 0; v < num_vars; v++) {
        rc_bitset_set(&full, v);
    }

    // Backward fixpoint. Round-robin in reverse block order (a backward analysis converges fastest walking
    // predecessors last); tiny CFGs, so no worklist. live_out = union of successors' live_in (+ full if the
    // block's exit is unknown); live_in = use union (live_out minus def).
    rc_bitset new_out = {0}; rc_bitset_resize(&new_out, num_vars, &scratch);
    rc_bitset new_in  = {0}; rc_bitset_resize(&new_in,  num_vars, &scratch);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = nb; bi-- > 0; ) {
            basic_block block = rc_array_basic_block_get(&g.blocks, bi);
            rc_bitset_reset(&new_out);
            for (uint32_t k = 0; k < block.succ_count; k++) {
                rc_bitset_union(&new_out, &lv.live_in[cfg_succ(g, block, k)]);
            }
            if (block.unknown_succ) {
                rc_bitset_union(&new_out, &full);
            }
            rc_bitset_copy(&new_in, &new_out);
            for (uint32_t v = rc_bitset_get_first_set(&def[bi]); v != RC_INDEX_NONE;
                 v = rc_bitset_get_next_set(&def[bi], v + 1)) {
                rc_bitset_clear(&new_in, v);
            }
            rc_bitset_union(&new_in, &use[bi]);
            if (!rc_bitset_is_equal(&new_out, &lv.live_out[bi]) || !rc_bitset_is_equal(&new_in, &lv.live_in[bi])) {
                changed = true;
                rc_bitset_copy(&lv.live_out[bi], &new_out);
                rc_bitset_copy(&lv.live_in[bi], &new_in);
            }
        }
    }

    // Interference, per instruction. Sweep each block backward from its live-out: at a def of `a`, `a`
    // interferes with everything else live at that point; then advance the live set to before the instruction
    // (remove the def, add the use). All the "in reuses in's byte", "keep hits the whole callee footprint"
    // legality falls out of these overlaps - no special cases.
    rc_bitset live = {0}; rc_bitset_resize(&live, num_vars, &scratch);
    for (uint32_t b = 0; b < nb; b++) {
        basic_block block = rc_array_basic_block_get(&g.blocks, b);
        rc_bitset_copy(&live, &lv.live_out[b]);
        for (uint32_t k = block.num_insns; k-- > 0; ) {
            zp_insn n = rc_view_zp_insn_get(insns, block.first_insn + k);
            uint32_t d = insn_def(n);
            if (d != RC_INDEX_NONE) {
                for (uint32_t o = rc_bitset_get_first_set(&live); o != RC_INDEX_NONE;
                     o = rc_bitset_get_next_set(&live, o + 1)) {
                    add_edge(lv.interfere, d, o);
                }
                rc_bitset_clear(&live, d);
            }
            uint32_t u = insn_use(n);
            if (u != RC_INDEX_NONE) {
                rc_bitset_set(&live, u);
            }
        }
    }
    // The routine's inputs are all simultaneously live at entry (the spec's synthetic ENTRY def = In(R)), so
    // they pairwise interfere - an edge no real-instruction def would create for a read-only input.
    if (entry_block < nb) {
        rc_bitset *lin = &lv.live_in[entry_block];
        for (uint32_t a = rc_bitset_get_first_set(lin); a != RC_INDEX_NONE; a = rc_bitset_get_next_set(lin, a + 1)) {
            for (uint32_t b = rc_bitset_get_next_set(lin, a + 1); b != RC_INDEX_NONE; b = rc_bitset_get_next_set(lin, b + 1)) {
                add_edge(lv.interfere, a, b);
            }
        }
    }

    // Classification. Scan every instruction for whether each vreg is ever read / written, then combine with
    // its live-in-at-entry status (see vreg_class). input wins over temp/output when a var is both live-in
    // and rewritten (an in-out param); C3 will separate those with real caller liveness.
    rc_bitset seen_read  = {0}; rc_bitset_resize(&seen_read,  num_vars, &scratch);
    rc_bitset seen_write = {0}; rc_bitset_resize(&seen_write, num_vars, &scratch);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.vreg == RC_INDEX_NONE) {
            continue;
        }
        if (n.rw & vref_read)  { rc_bitset_set(&seen_read,  n.vreg); }
        if (n.rw & vref_write) { rc_bitset_set(&seen_write, n.vreg); }
    }
    for (uint32_t v = 0; v < num_vars; v++) {
        bool read    = rc_bitset_is_set(&seen_read, v);
        bool written = rc_bitset_is_set(&seen_write, v);
        bool live_in = entry_block < nb && rc_bitset_is_set(&lv.live_in[entry_block], v);
        if (!read && !written) {
            lv.classes[v] = vreg_class_unused;
        } else if (live_in) {
            lv.classes[v] = vreg_class_input;      // read before any write on some path from entry
        } else if (written && !read) {
            lv.classes[v] = vreg_class_output;     // written, never read inside -> escapes to the caller
        } else {
            lv.classes[v] = vreg_class_temp;       // written and read inside
        }
    }

    return lv;
}

bool liveness_interferes(const liveness *lv, uint32_t a, uint32_t b)
{
    RC_ASSERT(lv != NULL);
    return a != b && a < lv->num_vars && b < lv->num_vars && rc_bitset_is_set(&lv->interfere[a], b);
}

vreg_class liveness_class_of(const liveness *lv, uint32_t vreg)
{
    RC_ASSERT(lv != NULL);
    return vreg < lv->num_vars ? lv->classes[vreg] : vreg_class_unused;
}

bool liveness_is_live_in(const liveness *lv, uint32_t block, uint32_t vreg)
{
    RC_ASSERT(lv != NULL);
    return block < lv->num_blocks && vreg < lv->num_vars && rc_bitset_is_set(&lv->live_in[block], vreg);
}

bool liveness_is_live_out(const liveness *lv, uint32_t block, uint32_t vreg)
{
    RC_ASSERT(lv != NULL);
    return block < lv->num_blocks && vreg < lv->num_vars && rc_bitset_is_set(&lv->live_out[block], vreg);
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Append one instruction with an explicit vreg + rw and return the running pc, so a test writes a stream as a
// column of touches without hand-tracking addresses. vreg RC_INDEX_NONE / rw vref_none = a non-var insn.
static uint32_t touch(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, zp_flow flow, uint32_t target,
                      uint32_t vreg, uint8_t rw, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = flow, .rw = rw, .vreg = vreg, .target = target,
                   .target_scope = RC_INDEX_NONE, .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc + size;
}

// The spec's `mul` routine: vreg 0 = in1 (read-only), 1 = tmp (write then read), 2 = out1 (write-only).
//   LDA in1 : ASL A : STA tmp : LDA in1 : CLC : ADC tmp : STA out1 : RTS
static rc_array_zp_insn build_mul(rc_arena *arena)
{
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,          arena);   // LDA in1
    pc = touch(&insns, pc, 1, zp_flow_normal, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, arena); // ASL A
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write,         arena);   // STA tmp
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,          arena);   // LDA in1
    pc = touch(&insns, pc, 1, zp_flow_normal, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, arena); // CLC
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_read,          arena);   // ADC tmp
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 2, vref_write,         arena);   // STA out1
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, arena); // RTS
    (void) pc;
    return insns;
}

RC_TEST(liveness, mul_interference)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = build_mul(&arena);
    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, 3, 0, &arena, scratch);

    // in1 overlaps tmp (both live at n3..n4) -> they interfere. But in1 dies before out1 is born, and tmp
    // dies before out1 is born, so out1 reuses their bytes - no interference. This IS the reuse fact.
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));    // in1 - tmp
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 2));   // in1 - out1
    RC_CHECK_FALSE(liveness_interferes(&lv, 1, 2));   // tmp - out1

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, mul_classification)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = build_mul(&arena);
    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, 3, 0, &arena, scratch);

    RC_CHECK_TRUE(liveness_class_of(&lv, 0) == vreg_class_input);    // in1 read before written
    RC_CHECK_TRUE(liveness_class_of(&lv, 1) == vreg_class_temp);     // tmp born and consumed inside
    RC_CHECK_TRUE(liveness_class_of(&lv, 2) == vreg_class_output);   // out1 written, never read inside
    RC_CHECK_TRUE(liveness_is_live_in(&lv, 0, 0));                   // in1 is live on entry...
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 1));                  // ...tmp and out1 are not
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 2));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, disjoint_locals_reuse_a_byte)
{
    // The user's headline case: STA var1 : LDA var1 : STA var2 : LDA var2. var1 dies before var2 is born, so
    // their live ranges are disjoint and they can share one zero-page byte.
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);   // STA var1
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA var1
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA var2
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_read,  &arena);   // LDA var2
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, 2, 0, &arena, scratch);
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 1));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, loop_carries_value_across_back_edge)
{
    // A back-edge forces the fixpoint to iterate: v0 is read at the loop top and never written, so it must be
    // live-in at the top on every iteration (a loop-invariant input), which only holds once live-out of the
    // branch block feeds live-in of the top back in.
    //   2000 .loop LDA v0   (read v0)          <- branch target
    //   2002       STA v1   (write v1)
    //   2004       BNE 2000 (branch back; fall-through 2006)
    //   2006       RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_read,  &arena);   // LDA v0
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, vref_write, &arena);   // STA v1
    pc = touch(&insns, pc, 2, zp_flow_branch, 0x2000,        RC_INDEX_NONE, vref_none, &arena);   // BNE loop
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, 2, 0, &arena, scratch);
    RC_CHECK_TRUE(liveness_is_live_in(&lv, 0, 0));    // v0 live around the loop
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 0));   // and still live out (back-edge carries it)

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, unknown_successor_keeps_everything_live)
{
    // The conservative taint end to end: a var defined just before an indirect jump must be assumed live out
    // (the computed target might read it), so it interferes with every other var - reuse is refused rather
    // than an unsound allocation emitted.
    //   2000 STA v0    (write v0)
    //   2002 JMP (ind) (jump, target unknown -> unknown_succ)
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);
    uint32_t pc = 0x2000;
    pc = touch(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, vref_write, &arena);   // STA v0
    pc = touch(&insns, pc, 3, zp_flow_jump,   RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // JMP (ind)
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, 2, 0, &arena, scratch);   // v1 exists in the id space, unused
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 0));   // v0 forced live out by the taint...
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 1));   // ...as is v1
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));    // so v0's def collides with v1 - no reuse across it

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
