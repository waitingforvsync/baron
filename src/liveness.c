#include "liveness.h"

#include "richc/macros.h"


// The small set algebra the fixpoint needs (copy / union / equality) lives in richc's rc_bitset now. The
// dataflow sets are byte-wide (nbytes bits), the public results variable-wide (num_vars bits); sets only
// ever meet others of their own width, so the equal-width preconditions hold throughout.

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

// ---- the byte id space ----
// Liveness is tracked per BYTE, not per variable: vreg v's bytes occupy [base[v], base[v] + width[v]) in a
// dense byte id space, and the dataflow sets below are byte sets. This is what makes partial writes come
// out right for multi-byte variables: a store redefines exactly the byte it hits, so `STA ptr` alone
// leaves the MSB's old value live straight through it, while a `STA ptr : STA ptr+1` pair accumulates into
// a full kill through the fixpoint (across blocks too - each byte's kill propagates independently). The
// PUBLIC results stay at variable level: the interference rows and live-in/out sets are projected back,
// a variable being live iff any of its bytes is - the allocator places whole variables, so that is the
// granularity it consumes.

// Which of its variable's bytes an instruction touches, as offset windows within the variable.
//   read  - bytes whose OLD value the instruction consumes: the addressed byte (two for an indirect
//           pointer deref), or the whole variable for an indexed / unknown-offset access.
//   write - bytes the instruction PROVABLY redefines: a direct store's one byte. An indexed or
//           unknown-offset store redefines nothing provable (it still pins the variable for interference).
typedef struct touch_window {
    uint32_t read_first, read_count;
    uint32_t write_first, write_count;
} touch_window;

static touch_window insn_window(zp_insn n, uint16_t width)
{
    touch_window w = {0};
    bool direct = !n.var_indexed && n.var_offset != RC_INDEX_NONE && n.var_offset < width;
    if (n.rw & vref_read) {
        uint32_t access = n.var_indirect ? 2u : 1u;
        if (direct && n.var_offset + access <= width) {
            w.read_first = n.var_offset;
            w.read_count = access;
        }
        else {
            w.read_first = 0;   // indexed / unknown offset: any byte's old value may be consumed
            w.read_count = width;
        }
    }
    if ((n.rw & vref_write) && direct) {
        w.write_first = n.var_offset;
        w.write_count = 1;   // a 6502 store writes one byte
    }
    return w;
}

static void add_edge(rc_bitset *interfere, uint32_t a, uint32_t b)
{
    if (a != b) {
        rc_bitset_set(&interfere[a], b);
        rc_bitset_set(&interfere[b], a);
    }
}

liveness liveness_analyze(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_var vars,
                          uint32_t entry_block, rc_arena *arena, rc_arena scratch)
{
    uint32_t num_vars = vars.num;
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

    // Stand up the byte id space: contiguous per-variable byte runs, plus the byte -> owning-vreg map the
    // projections read.
    uint32_t *base = rc_arena_alloc_type(&scratch, uint32_t, num_vars);
    uint32_t nbytes = 0;
    for (uint32_t v = 0; v < num_vars; v++) {
        base[v] = nbytes;
        nbytes += rc_view_zp_var_get(vars, v).width;
    }
    uint32_t *owner = rc_arena_alloc_type(&scratch, uint32_t, nbytes);
    for (uint32_t v = 0; v < num_vars; v++) {
        for (uint32_t k = 0; k < rc_view_zp_var_get(vars, v).width; k++) {
            owner[base[v] + k] = v;
        }
    }

    // Each call site's callee entry blocks, resolved once (CANCALL overrides included). A call is treated
    // as a USE of its callee's live-in set - the callee's inputs - which is what gives an argument stored
    // by the caller a live range reaching the JSR, so nothing can be coloured over it in between. The
    // callee's live-in is itself a fixpoint variable, so the injection happens inside the dataflow loop
    // and propagates transitively through call chains. The two arms that resolve to no blocks inject
    // nothing: an external callee touches none of our bytes, and a computed unannotated call is already
    // the user's responsibility (CANCALL declares the targets whose inputs then count).
    rc_array_u32 *callees = rc_arena_alloc_zero_type(&scratch, rc_array_u32, insns.num);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn n = rc_view_zp_insn_get(insns, i);
        if (n.flow == zp_flow_call) {
            callees[i] = cfg_call_targets(g, cflows, n, &scratch).blocks;
        }
    }

    // A full set for the unknown_succ taint: a block whose control may leave to an address we cannot model
    // must assume every byte is live out (the computed target might read any of them).
    rc_bitset full = {0};
    rc_bitset_resize(&full, nbytes, &scratch);
    for (uint32_t i = 0; i < nbytes; i++) {
        rc_bitset_set(&full, i);
    }

    // Backward fixpoint over the byte sets. Round-robin in reverse block order (a backward analysis
    // converges fastest walking predecessors last); tiny CFGs, so no worklist. live_out = union of
    // successors' live_in (+ full if the block's exit is unknown); live_in comes from walking the block's
    // instructions backward - a call adds its callees' live-in, a write removes its provable bytes, a read
    // adds its bytes. (The walk replaces the use/kill set equations: a call's effect depends on another
    // block's evolving live-in, which a precomputed use set cannot express.)
    rc_bitset *bin  = make_rows(nb, nbytes, &scratch);
    rc_bitset *bout = make_rows(nb, nbytes, &scratch);
    rc_bitset new_out = {0}; rc_bitset_resize(&new_out, nbytes, &scratch);
    rc_bitset new_in  = {0}; rc_bitset_resize(&new_in,  nbytes, &scratch);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = nb; bi-- > 0; ) {
            basic_block block = rc_array_basic_block_get(&g.blocks, bi);
            rc_bitset_reset(&new_out);
            for (uint32_t k = 0; k < block.succ_count; k++) {
                rc_bitset_union(&new_out, &bin[cfg_succ(g, block, k)]);
            }
            if (block.unknown_succ) {
                rc_bitset_union(&new_out, &full);
            }
            rc_bitset_copy(&new_in, &new_out);
            for (uint32_t k = block.num_insns; k-- > 0; ) {
                uint32_t ni = block.first_insn + k;
                zp_insn  n  = rc_view_zp_insn_get(insns, ni);
                for (uint32_t c = 0; c < callees[ni].view.num; c++) {
                    rc_bitset_union(&new_in, &bin[rc_array_u32_get(&callees[ni], c)]);
                }
                if (n.vreg == RC_INDEX_NONE) {
                    continue;
                }
                touch_window w = insn_window(n, rc_view_zp_var_get(vars, n.vreg).width);
                for (uint32_t i = 0; i < w.write_count; i++) {
                    rc_bitset_clear(&new_in, base[n.vreg] + w.write_first + i);
                }
                for (uint32_t i = 0; i < w.read_count; i++) {
                    rc_bitset_set(&new_in, base[n.vreg] + w.read_first + i);
                }
            }
            if (!rc_bitset_is_equal(&new_out, &bout[bi]) || !rc_bitset_is_equal(&new_in, &bin[bi])) {
                changed = true;
                rc_bitset_copy(&bout[bi], &new_out);
                rc_bitset_copy(&bin[bi], &new_in);
            }
        }
    }

    // Project the byte sets to the VARIABLE-level results the callers consume: a variable is live iff any
    // of its bytes is.
    for (uint32_t b = 0; b < nb; b++) {
        for (uint32_t i = rc_bitset_get_first_set(&bin[b]); i != RC_INDEX_NONE;
             i = rc_bitset_get_next_set(&bin[b], i + 1)) {
            rc_bitset_set(&lv.live_in[b], owner[i]);
        }
        for (uint32_t i = rc_bitset_get_first_set(&bout[b]); i != RC_INDEX_NONE;
             i = rc_bitset_get_next_set(&bout[b], i + 1)) {
            rc_bitset_set(&lv.live_out[b], owner[i]);
        }
    }

    // Interference, per instruction. Sweep each block backward from its byte live-out: at any WRITE the
    // variable is pinned to its bytes, so it interferes with the owner of every other live byte (indexed
    // and unknown-offset stores pin too, even though they kill nothing); then advance the live set to
    // before the instruction - remove the provably-rewritten bytes, add the read ones. A lone `STA ptr`
    // removes only the LSB's byte, so the MSB keeps the pointer live and overlap with it still registers;
    // once the MSB's store joins it the whole variable goes dead and the range genuinely ends. All the "in
    // reuses in's byte", "keep hits the whole callee footprint" legality falls out of these overlaps.
    rc_bitset live = {0}; rc_bitset_resize(&live, nbytes, &scratch);
    for (uint32_t b = 0; b < nb; b++) {
        basic_block block = rc_array_basic_block_get(&g.blocks, b);
        rc_bitset_copy(&live, &bout[b]);
        for (uint32_t k = block.num_insns; k-- > 0; ) {
            uint32_t ni = block.first_insn + k;
            zp_insn  n  = rc_view_zp_insn_get(insns, ni);
            // A call consumes its callees' inputs, so they are live from here back to their stores - the
            // range that lets an argument's bytes register overlap with anything written in between.
            for (uint32_t c = 0; c < callees[ni].view.num; c++) {
                rc_bitset_union(&live, &bin[rc_array_u32_get(&callees[ni], c)]);
            }
            if (n.vreg == RC_INDEX_NONE) {
                continue;
            }
            touch_window w = insn_window(n, rc_view_zp_var_get(vars, n.vreg).width);
            if (n.rw & vref_write) {
                for (uint32_t o = rc_bitset_get_first_set(&live); o != RC_INDEX_NONE;
                     o = rc_bitset_get_next_set(&live, o + 1)) {
                    add_edge(lv.interfere, n.vreg, owner[o]);
                }
                for (uint32_t i = 0; i < w.write_count; i++) {
                    rc_bitset_clear(&live, base[n.vreg] + w.write_first + i);
                }
            }
            for (uint32_t i = 0; i < w.read_count; i++) {
                rc_bitset_set(&live, base[n.vreg] + w.read_first + i);
            }
        }
    }
    // The routine's inputs are all simultaneously live at entry (the spec's synthetic ENTRY def = In(R)), so
    // they pairwise interfere - an edge no real-instruction def would create for a read-only input.
    if (entry_block < nb) {
        rc_bitset *lin = &bin[entry_block];
        for (uint32_t a = rc_bitset_get_first_set(lin); a != RC_INDEX_NONE; a = rc_bitset_get_next_set(lin, a + 1)) {
            for (uint32_t b = rc_bitset_get_next_set(lin, a + 1); b != RC_INDEX_NONE; b = rc_bitset_get_next_set(lin, b + 1)) {
                add_edge(lv.interfere, owner[a], owner[b]);
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

// A registry of `n` one-byte variables (vreg i = vars[i]) - the common case; a test that needs a wider
// variable builds its own registry.
static rc_view_zp_var width1_vars(uint32_t n, rc_arena *arena)
{
    rc_array_zp_var vars = rc_array_zp_var_make(n ? n : 1, arena);
    for (uint32_t i = 0; i < n; i++) {
        rc_array_zp_var_push(&vars, (zp_var) {.name = RC_STR("v"), .scope = 0, .width = 1, .def = (cursor) {0}},
                             arena);
    }
    return vars.view;
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
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(3, &arena), 0, &arena, scratch);

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
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(3, &arena), 0, &arena, scratch);

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
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);
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
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);
    RC_CHECK_TRUE(liveness_is_live_in(&lv, 0, 0));    // v0 live around the loop
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 0));   // and still live out (back-edge carries it)

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

// touch() with an explicit byte window: `offset` into the variable, `indirect` for a 2-byte pointer deref.
static uint32_t touch_at(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, uint32_t vreg, uint8_t rw,
                         uint32_t offset, bool indirect, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = zp_flow_normal, .rw = rw, .vreg = vreg,
                   .var_offset = offset, .var_indirect = indirect, .target = RC_INDEX_NONE,
                   .target_scope = RC_INDEX_NONE, .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc + size;
}

// A 2-byte pointer (vreg 0) and a 1-byte temp (vreg 1), for the partial-write tests.
static rc_view_zp_var pointer_and_temp(rc_arena *arena)
{
    rc_array_zp_var vars = rc_array_zp_var_make(2, arena);
    rc_array_zp_var_push(&vars, (zp_var) {.name = RC_STR("p"), .scope = 0, .width = 2, .def = (cursor) {0}}, arena);
    rc_array_zp_var_push(&vars, (zp_var) {.name = RC_STR("t"), .scope = 0, .width = 1, .def = (cursor) {0}}, arena);
    return vars.view;
}

RC_TEST(liveness, partial_write_keeps_a_pointer_live)
{
    // The demo.6502 shape: a 2-byte pointer p whose MSB is written once up front, while a loop rewrites
    // only the LSB before each deref. The LSB store redefines one byte - the MSB's old value flows through
    // it - so p stays live around the whole loop and must interfere with the loop temp t, or an
    // overlapping allocation would clobber the MSB. (A per-variable kill rule treated the LSB store as a
    // full redefinition, severed the range, and let t share p's bytes - the demo's broken globe.)
    //   2000        STA p+1   (the init)
    //   2002 .loop  STA p     (LSB only)
    //   2004        LDA (p),Y (deref: reads both bytes)
    //   2006        STA t : LDA t
    //   200A        BNE loop
    //   200C        RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch_at(&insns, pc, 2, 0, vref_write, 1, false, &arena);   // STA p+1
    pc = touch_at(&insns, pc, 2, 0, vref_write, 0, false, &arena);   // STA p
    pc = touch_at(&insns, pc, 2, 0, vref_read,  0, true,  &arena);   // LDA (p),Y
    pc = touch_at(&insns, pc, 2, 1, vref_write, 0, false, &arena);   // STA t
    pc = touch_at(&insns, pc, 2, 1, vref_read,  0, false, &arena);   // LDA t
    pc = touch(&insns, pc, 2, zp_flow_branch, 0x2002, RC_INDEX_NONE, vref_none, &arena);   // BNE loop
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, pointer_and_temp(&arena), 0, &arena, scratch);
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));                     // the MSB rides through STA p
    RC_CHECK_TRUE(liveness_is_live_in(&lv, cfg_block_at(g, 0, 0x2002), 0));   // p live around the loop

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(liveness, full_byte_rewrite_kills_a_pointer)
{
    // The contrast: when BOTH bytes are stored before the deref, the per-byte kills accumulate into a
    // full redefinition, the old range genuinely ends, and a temp that dies beforehand still shares the
    // pointer's bytes. (So yes - partial writes across the width of a variable are tracked, and a total
    // rewrite is a kill.)
    //   2000  STA t : LDA t          (t's whole life)
    //   2004  STA p : STA p+1        (a full rewrite, byte by byte)
    //   2008  LDA (p),Y : RTS
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct from arena: by-value scratch must not share backing
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);
    uint32_t pc = 0x2000;
    pc = touch_at(&insns, pc, 2, 1, vref_write, 0, false, &arena);   // STA t
    pc = touch_at(&insns, pc, 2, 1, vref_read,  0, false, &arena);   // LDA t
    pc = touch_at(&insns, pc, 2, 0, vref_write, 0, false, &arena);   // STA p
    pc = touch_at(&insns, pc, 2, 0, vref_write, 1, false, &arena);   // STA p+1
    pc = touch_at(&insns, pc, 2, 0, vref_read,  0, true,  &arena);   // LDA (p),Y
    pc = touch(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, vref_none, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, pointer_and_temp(&arena), 0, &arena, scratch);
    RC_CHECK_FALSE(liveness_interferes(&lv, 0, 1));   // t is dead before the rewrite begins - reuse is safe
    RC_CHECK_FALSE(liveness_is_live_in(&lv, 0, 0));   // and nothing of p's old value flows in

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
    liveness lv = liveness_analyze(g, insns.view, (rc_view_zp_cflow) {0}, width1_vars(2, &arena), 0, &arena, scratch);   // v1 exists in the id space, unused
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 0));   // v0 forced live out by the taint...
    RC_CHECK_TRUE(liveness_is_live_out(&lv, 0, 1));   // ...as is v1
    RC_CHECK_TRUE(liveness_interferes(&lv, 0, 1));    // so v0's def collides with v1 - no reuse across it

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
