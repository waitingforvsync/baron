#include "footprint.h"

#include "richc/macros.h"


// Everything the recursion needs, so the per-routine walk stays a plain function of (context, entry). touched
// and on_stack are SHARED across the whole recursion (by pointer); the per-routine BFS sets are taken fresh
// from the by-value scratch at each level.
typedef struct fp_ctx {
    cfg              g;
    rc_view_zp_insn  insns;
    rc_view_zp_cflow cflows;   // CANCALL overrides: a JSR's declared target set
    rc_bitset       *touched;   // accumulate vregs here (in the caller's arena)
    rc_bitset       *killed;    // vregs given a write-only def here (a fresh value, per the footprint doc)
    rc_bitset       *on_stack;  // routine-entry blocks currently being computed - a revisit is recursion
    bool            *unknown;
    bool            *recursive;
} fp_ctx;

// fp_visit and fp_visit_call are mutually recursive (a call reaches a routine, whose blocks make more calls).
static void fp_visit(fp_ctx *c, uint32_t entry, rc_arena scratch);

// Descend into what call site `n` reaches. cfg_call_targets applies the whole target policy - a CANCALL
// override, label resolution (so a JSR into another section finds the right block), external arms
// contributing nothing - and flags the one shape we cannot follow, a computed unannotated call.
static void fp_visit_call(fp_ctx *c, zp_insn n, rc_arena scratch)
{
    call_targets t = cfg_call_targets(c->g, c->cflows, n, &scratch);
    if (t.unknown) {
        *c->unknown = true;
    }
    for (uint32_t i = 0; i < t.blocks.view.num; i++) {
        fp_visit(c, rc_array_u32_get(&t.blocks, i), scratch);
    }
}

// Walk the routine entered at `entry`: gather its own blocks' vreg touches (BFS over intraprocedural edges),
// and recurse into each JSR target. `scratch` is by value, so each recursion level allocates its BFS sets in
// fresh space after its parent's and they are reclaimed on return.
static void fp_visit(fp_ctx *c, uint32_t entry, rc_arena scratch)
{
    if (rc_bitset_is_set(c->on_stack, entry)) {
        *c->recursive = true;   // a call cycle: this routine is (transitively) already being computed
        return;
    }
    rc_bitset_set(c->on_stack, entry);

    uint32_t nb = c->g.blocks.num;
    rc_bitset seen = {0};
    rc_bitset_resize(&seen, nb, &scratch);
    uint32_t *stack = rc_arena_alloc_type(&scratch, uint32_t, nb);
    uint32_t  sp    = 0;
    stack[sp++] = entry;
    rc_bitset_set(&seen, entry);

    while (sp > 0) {
        uint32_t bi = stack[--sp];
        basic_block blk = rc_array_basic_block_get(&c->g.blocks, bi);
        if (blk.unknown_succ) {
            *c->unknown = true;   // a callee that leaves via a computed jump: footprint cannot be bounded
        }
        for (uint32_t k = 0; k < blk.num_insns; k++) {
            zp_insn n = rc_view_zp_insn_get(c->insns, blk.first_insn + k);
            if (n.vreg != RC_INDEX_NONE && !n.var_kill) {
                // A DISCARD marker is not a touch: it stores nothing, so a caller's variable sharing the
                // byte is safe across a callee that merely discards - and it must not feed `killed`, whose
                // job is spotting FRESH per-level values across recursion.
                rc_bitset_set(c->touched, n.vreg);
                // A write with no accompanying read is a fresh assignment - the value came from nowhere prior.
                // If the recursion does this and holds it live across itself, one static byte cannot serve it.
                if ((n.rw & vref_write) && !(n.rw & vref_read)) {
                    rc_bitset_set(c->killed, n.vreg);
                }
            }
            if (n.flow == zp_flow_call) {
                fp_visit_call(c, n, scratch);
            }
        }
        for (uint32_t s = 0; s < blk.succ_count; s++) {
            uint32_t succ = cfg_succ(c->g, blk, s);
            if (!rc_bitset_is_set(&seen, succ)) {
                rc_bitset_set(&seen, succ);
                stack[sp++] = succ;
            }
        }
    }

    rc_bitset_clear(c->on_stack, entry);
}

footprint footprint_compute(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, uint32_t entry_block,
                            uint32_t num_vars, rc_arena *arena, rc_arena scratch)
{
    footprint fp = {
        .touched      = {0},
        .killed       = {0},
        .unknown_call = false,
        .recursive    = false,
    };
    rc_bitset_resize(&fp.touched, num_vars, arena);
    rc_bitset_resize(&fp.killed, num_vars, arena);
    if (entry_block >= g.blocks.num || num_vars == 0) {
        return fp;
    }
    rc_bitset on_stack = {0};
    rc_bitset_resize(&on_stack, g.blocks.num, &scratch);
    fp_ctx c = {
        .g         = g,
        .insns     = insns,
        .cflows    = cflows,
        .touched   = &fp.touched,
        .killed    = &fp.killed,
        .on_stack  = &on_stack,
        .unknown   = &fp.unknown_call,
        .recursive = &fp.recursive,
    };
    fp_visit(&c, entry_block, scratch);
    return fp;
}

footprint footprint_of_call(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, zp_insn call,
                            uint32_t num_vars, rc_arena *arena, rc_arena scratch)
{
    footprint fp = {
        .touched      = {0},
        .killed       = {0},
        .unknown_call = false,
        .recursive    = false,
    };
    rc_bitset_resize(&fp.touched, num_vars, arena);
    rc_bitset_resize(&fp.killed, num_vars, arena);
    if (num_vars == 0) {
        return fp;
    }
    rc_bitset on_stack = {0};
    rc_bitset_resize(&on_stack, g.blocks.num, &scratch);
    fp_ctx c = {
        .g         = g,
        .insns     = insns,
        .cflows    = cflows,
        .touched   = &fp.touched,
        .killed    = &fp.killed,
        .on_stack  = &on_stack,
        .unknown   = &fp.unknown_call,
        .recursive = &fp.recursive,
    };
    fp_visit_call(&c, call, scratch);
    return fp;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// Append one instruction (vreg + flow + target); returns the running pc so a test writes a stream as a column.
static uint32_t fp_push(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, zp_flow flow, uint32_t target,
                        uint32_t vreg, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = flow, .rw = vref_none, .vreg = vreg,
                   .var_scope = 0, .var_def = (cursor) {0}, .target = target,
                   .target_scope = RC_INDEX_NONE, .target_def = cursor_none(), .at = (cursor) {0}},
        arena);
    return pc + size;
}

RC_TEST(footprint, transitive_touch_through_calls)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000 main:  LDA v0 : JSR 2006 : RTS      (sub is emitted contiguously at 2006)
    //   2006 sub:   LDA v1 : RTS
    // Footprint of main = {v0, v1} (its own v0 plus the callee's v1); footprint of sub = {v1}.
    uint32_t pc = 0x2000;
    pc = fp_push(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, &arena);   // LDA v0
    pc = fp_push(&insns, pc, 3, zp_flow_call,   0x2006,        RC_INDEX_NONE, &arena);   // JSR 2006
    pc = fp_push(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, &arena);   // RTS
    pc = fp_push(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 1, &arena);   // LDA v1  @2006
    pc = fp_push(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_view_zp_cflow none = {0};   // no annotations in these tests
    footprint main_fp = footprint_compute(g, insns.view, none, cfg_block_at(g, 0, 0x2000), 2, &arena, scratch);
    RC_CHECK_FALSE(main_fp.unknown_call);
    RC_CHECK_FALSE(main_fp.recursive);
    RC_CHECK_TRUE(rc_bitset_is_set(&main_fp.touched, 0));   // v0 (own)
    RC_CHECK_TRUE(rc_bitset_is_set(&main_fp.touched, 1));   // v1 (callee's)

    footprint sub_fp = footprint_compute(g, insns.view, none, cfg_block_at(g, 0, 0x2006), 2, &arena, scratch);
    RC_CHECK_FALSE(rc_bitset_is_set(&sub_fp.touched, 0));   // main's v0 is NOT sub's footprint
    RC_CHECK_TRUE(rc_bitset_is_set(&sub_fp.touched, 1));

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(footprint, recursion_is_flagged)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);

    //   2000 r: LDA v0 : JSR 2000 : RTS   - r calls itself
    uint32_t pc = 0x2000;
    pc = fp_push(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, 0, &arena);   // LDA v0
    pc = fp_push(&insns, pc, 3, zp_flow_call,   0x2000,        RC_INDEX_NONE, &arena);   // JSR 2000 (self)
    pc = fp_push(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_view_zp_cflow none = {0};
    footprint fp = footprint_compute(g, insns.view, none, cfg_block_at(g, 0, 0x2000), 1, &arena, scratch);
    RC_CHECK_TRUE(fp.recursive);
    RC_CHECK_TRUE(rc_bitset_is_set(&fp.touched, 0));   // still terminates and gathers v0

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(footprint, unknown_call_target_is_flagged)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);

    //   2000 JSR <computed>  (target unknown) : RTS
    uint32_t pc = 0x2000;
    pc = fp_push(&insns, pc, 3, zp_flow_call,   RC_INDEX_NONE, RC_INDEX_NONE, &arena);   // JSR unknown
    pc = fp_push(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_view_zp_cflow none = {0};
    footprint fp = footprint_compute(g, insns.view, none, cfg_block_at(g, 0, 0x2000), 1, &arena, scratch);
    RC_CHECK_TRUE(fp.unknown_call);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(footprint, external_call_has_empty_footprint)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();
    rc_array_zp_insn insns = rc_array_zp_insn_make(4, &arena);

    //   2000 JSR FFEE : RTS - a CONSTANT target off the assembled stream is a call OUT of the program (an OS
    // entry). External code touches none of our variables, so the call is trackable with an EMPTY footprint -
    // not an unknown_call refusal.
    uint32_t pc = 0x2000;
    pc = fp_push(&insns, pc, 3, zp_flow_call,   0xFFEE,        RC_INDEX_NONE, &arena);   // JSR &FFEE
    pc = fp_push(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, (rc_view_zp_entry) {0}, &arena, scratch);
    rc_view_zp_cflow none = {0};
    footprint fp = footprint_compute(g, insns.view, none, cfg_block_at(g, 0, 0x2000), 1, &arena, scratch);
    RC_CHECK_FALSE(fp.unknown_call);
    RC_CHECK_FALSE(rc_bitset_is_set(&fp.touched, 0));   // the OS does not touch our vreg

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
