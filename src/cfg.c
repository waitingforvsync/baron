#include "cfg.h"

#include "richc/bitset.h"
#include "richc/macros.h"


enum {
    cfg_addr_space = 0x10000,   // the 6502's 64 KB - the leader bitset spans one bit per address, PER section
};


// A resolved target location: which (section, pc) a control transfer lands on. `found` is false for a computed
// / unregistered / absent target (the caller then leaves the edge unknown).
typedef struct target_loc {
    uint32_t section;
    uint32_t pc;
    bool     found;
} target_loc;

// Where does `n`'s control-transfer target land? A named-label target (target_scope set) resolves through the
// markers to the label's OWN section - this is what crosses a section, because the label picks the bank the
// raw address never could. If it named no label, or a label with no marker (a bare constant), we fall back to
// the numeric target, resolved WITHIN n's own section only. A computed / absent target is `found = false`.
static target_loc resolve_target_loc(rc_view_zp_label labels, zp_insn n)
{
    if (n.target_scope != RC_INDEX_NONE) {
        for (uint32_t i = 0; i < labels.num; i++) {
            zp_label l = rc_view_zp_label_get(labels, i);
            if (l.scope == n.target_scope && cursor_is_equal(l.def, n.target_def)) {
                return (target_loc) {.section = l.section, .pc = l.pc, .found = true};
            }
        }
        // A symbolic target with no marker (e.g. a `name = expr` constant, not a code label): treat the
        // resolved value as a bare address in n's own section, exactly like a numeric target.
    }
    if (n.target != RC_INDEX_NONE) {
        return (target_loc) {.section = n.section, .pc = n.target, .found = true};
    }
    return (target_loc) {.section = 0, .pc = 0, .found = false};
}

// The leader-set index for (section, pc): one 64 KB span of bits per section. Callers only pass sections that
// exist in the stream (< num_sections) and pcs < cfg_addr_space, so the index is always in range.
static uint32_t leader_index(uint32_t section, uint32_t pc)
{
    return section * cfg_addr_space + pc;
}

// Is (section, pc) the leader (entry) of a basic block? Leaders come from: the first instruction, every
// section boundary, every branch/jump/call TARGET, and the instruction after any branch / jump / return. A
// JSR does NOT boundary its fall-through (a call is in-block) but its target IS a leader (the callee's entry).
static bool addr_is_leader(const rc_bitset *leaders, uint32_t section, uint32_t pc)
{
    return pc < cfg_addr_space && rc_bitset_is_set(leaders, leader_index(section, pc));
}

// Mark (section, pc) a leader, ignoring a pc past 0xFFFF (a target past the address space, or the fall-through
// of the last byte of memory - neither names a real block).
static void mark_leader(rc_bitset *leaders, uint32_t section, uint32_t pc)
{
    if (pc < cfg_addr_space) {
        rc_bitset_set(leaders, leader_index(section, pc));
    }
}

// The block whose entry is exactly (section, pc), or RC_INDEX_NONE. The list is short (one per leader), so a
// linear scan is fine. Turns a fall-through / in-section target LOCATION into a successor block INDEX.
static uint32_t block_at(rc_view_basic_block blocks, uint32_t section, uint32_t pc)
{
    if (pc == RC_INDEX_NONE) {
        return RC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < blocks.num; i++) {
        basic_block b = rc_view_basic_block_get(blocks, i);
        if (b.section == section && b.pc == pc) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

uint32_t cfg_block_at(cfg g, uint32_t section, uint32_t pc)
{
    return block_at(g.blocks.view, section, pc);
}

uint32_t cfg_target_block(cfg g, zp_insn n)
{
    target_loc tl = resolve_target_loc(g.labels, n);
    return tl.found ? block_at(g.blocks.view, tl.section, tl.pc) : RC_INDEX_NONE;
}

uint32_t cfg_succ(cfg g, basic_block b, uint32_t i)
{
    RC_ASSERT(i < b.succ_count);
    return rc_view_u32_get(g.succs.view, b.succ_first + i);
}

// Append `succ_block` to the shared pool as one more successor of `block`. The pool grows independently of the
// blocks array, so a `block` pointer into the blocks array stays valid across this (only the pool relocates).
// A block's successors are pushed contiguously, so succ_first (set before the first push) + succ_count spans
// them.
static void add_succ(cfg *g, basic_block *block, uint32_t succ_block, rc_arena *arena)
{
    rc_array_u32_push(&g->succs, succ_block, arena);
    block->succ_count++;
}

// Does `cflows` carry an annotation of `kind` sited at `pc`? A linear scan - annotations are few.
static bool cflow_at(rc_view_zp_cflow cflows, zp_cflow_kind kind, uint32_t pc)
{
    for (uint32_t i = 0; i < cflows.num; i++) {
        zp_cflow cf = rc_view_zp_cflow_get(cflows, i);
        if (cf.kind == (uint8_t) kind && cf.site == pc) {
            return true;
        }
    }
    return false;
}

cfg cfg_build(rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_label labels,
              rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena != NULL);
    cfg result = {.blocks = rc_array_basic_block_make(16, arena), .succs = rc_array_u32_make(32, arena),
                  .labels = labels};
    if (insns.num == 0) {
        return result;
    }

    // The leader set is one 64 KB span per section, so the SAME address in two sections (paged banks) is two
    // distinct leaders. Size it from the highest section index in the stream (sections are numbered densely
    // from 0 in first-sighting order).
    uint32_t num_sections = 0;
    for (uint32_t i = 0; i < insns.num; i++) {
        uint32_t s = rc_view_zp_insn_get(insns, i).section;
        if (s + 1 > num_sections) {
            num_sections = s + 1;
        }
    }

    // Pass 1: find the block leaders. The first instruction always leads; a control-flow instruction marks its
    // resolved target LOCATION (branch/jump/call - possibly in another section, via a label) and, unless it is
    // a call, the location after it (branch/jump/return end a block, a call falls through in-block). We model
    // BOTH edges of a conditional branch - the sound default for liveness (a spurious edge only lengthens live
    // ranges, never shortens them; a MISSING edge is the only unsound case, which is why computed flow taints).
    rc_bitset leaders = {0};
    rc_bitset_resize(&leaders, num_sections * cfg_addr_space, &scratch);
    mark_leader(&leaders, rc_view_zp_insn_get(insns, 0).section, rc_view_zp_insn_get(insns, 0).pc);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn insn = rc_view_zp_insn_get(insns, i);
        uint32_t after = insn.pc + insn.size;   // the fall-through is always in THIS instruction's section
        target_loc tl = resolve_target_loc(labels, insn);
        switch (insn.flow) {
            case zp_flow_branch:
                if (tl.found) { mark_leader(&leaders, tl.section, tl.pc); }   // taken edge
                mark_leader(&leaders, insn.section, after);                   // fall-through edge
                break;
            case zp_flow_jump:
                if (tl.found) { mark_leader(&leaders, tl.section, tl.pc); }
                mark_leader(&leaders, insn.section, after);   // boundary (block after an unconditional jump)
                break;
            case zp_flow_call:
                if (tl.found) { mark_leader(&leaders, tl.section, tl.pc); }   // callee entry (via call graph)
                break;
            case zp_flow_return:
                mark_leader(&leaders, insn.section, after);    // boundary (block after a return)
                break;
            case zp_flow_normal:
            default:
                break;
        }
    }

    // Pass 2: cut the instruction stream into blocks. A new block starts at the first instruction, at every
    // SECTION change (which keeps each block single-section and its pc monotonic even as sections interleave),
    // and at any instruction that is a leader in its section; the block runs until the next such start.
    uint32_t current  = RC_INDEX_NONE;
    uint32_t prev_sec = 0;
    uint32_t prev_pc  = 0;
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn insn = rc_view_zp_insn_get(insns, i);
        // Within one section pc must be strictly forward - the property that keeps (section, pc) an unambiguous
        // block identity. It holds by construction (a section's org is fixed at open and its cursor only
        // advances), so this asserts the invariant rather than handling a violation.
        RC_ASSERT(i == 0 || insn.section != prev_sec || insn.pc > prev_pc);
        if (i == 0 || insn.section != prev_sec || addr_is_leader(&leaders, insn.section, insn.pc)) {
            current = rc_array_basic_block_push(
                &result.blocks,
                (basic_block) {.section = insn.section, .pc = insn.pc, .first_insn = i, .num_insns = 0,
                               .succ_first = 0, .succ_count = 0, .unknown_succ = false},
                arena);
        }
        rc_array_basic_block_at(&result.blocks, current)->num_insns++;
        prev_sec = insn.section;
        prev_pc  = insn.pc;
    }

    // Pass 3: wire successor edges from each block's LAST instruction's control-flow class into the shared
    // pool. A branch has two (fall-through + target), a jump one (target), a return none, and a call / normal
    // terminator falls through to the next block. Fall-through stays in the block's own section; the target is
    // resolved by resolve_target_loc (a named label may cross sections). Annotations adjust this: an
    // UNREACHABLE at a branch's fall-through prunes that edge; a CANJUMP at a computed JMP supplies its
    // (same-section) targets. A target that names no block and is not annotated yields the unknown_succ taint.
    for (uint32_t bi = 0; bi < result.blocks.num; bi++) {
        basic_block *block = rc_array_basic_block_at(&result.blocks, bi);
        zp_insn last = rc_view_zp_insn_get(insns, block->first_insn + block->num_insns - 1);
        uint32_t after = last.pc + last.size;
        target_loc tl  = resolve_target_loc(labels, last);
        block->succ_first = result.succs.num;
        switch (last.flow) {
            case zp_flow_branch: {
                // Not-taken (in-stream) fall-through, unless UNREACHABLE asserts control cannot reach it.
                uint32_t ft = block_at(result.blocks.view, last.section, after);
                if (ft != RC_INDEX_NONE && !cflow_at(cflows, zp_cflow_unreachable, after)) {
                    add_succ(&result, block, ft, arena);
                }
                uint32_t taken = tl.found ? block_at(result.blocks.view, tl.section, tl.pc) : RC_INDEX_NONE;
                if (taken != RC_INDEX_NONE) {
                    add_succ(&result, block, taken, arena);
                }
                else {
                    block->unknown_succ = true;   // taken target we cannot place -> conservative
                }
                break;
            }
            case zp_flow_jump: {
                uint32_t t = tl.found ? block_at(result.blocks.view, tl.section, tl.pc) : RC_INDEX_NONE;
                if (t != RC_INDEX_NONE) {
                    add_succ(&result, block, t, arena);   // a plain, resolved JMP
                }
                else {
                    // Computed / indirect JMP. If CANJUMP names its targets, wire each as a real edge (resolved
                    // in the jump's own section); a declared target we still cannot place keeps the taint.
                    bool annotated = false;
                    for (uint32_t i = 0; i < cflows.num; i++) {
                        zp_cflow cf = rc_view_zp_cflow_get(cflows, i);
                        if (cf.kind == zp_cflow_canjump && cf.site == last.pc) {
                            annotated = true;
                            uint32_t tb = block_at(result.blocks.view, last.section, cf.target);
                            if (tb != RC_INDEX_NONE) {
                                add_succ(&result, block, tb, arena);
                            }
                            else {
                                block->unknown_succ = true;
                            }
                        }
                    }
                    if (!annotated) {
                        block->unknown_succ = true;   // indirect/computed JMP, or a jump leaving the stream
                    }
                }
                break;
            }
            case zp_flow_return:
                break;   // a genuine return: no successor, fully known (an RTS-dispatch would need markup)
            case zp_flow_call:
            case zp_flow_normal:
            default: {
                // A call/normal terminator falls through in-stream (same section); a fall-through with no block
                // is the end of the program (or a routine falling off its end) - a clean end, not an unknown.
                uint32_t ft = block_at(result.blocks.view, last.section, after);
                if (ft != RC_INDEX_NONE) {
                    add_succ(&result, block, ft, arena);
                }
                break;
            }
        }
    }

    return result;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// A tiny builder for the tests: append one instruction, defaulting the fields a CFG test does not care about
// (vreg/rw/at). Returns the running pc so a test can chain instructions without hand-tracking addresses.
static uint32_t push_insn(rc_array_zp_insn *insns, uint32_t pc, uint16_t size, zp_flow flow,
                          uint32_t target, rc_arena *arena)
{
    rc_array_zp_insn_push(insns,
        (zp_insn) {.pc = pc, .size = size, .flow = flow, .rw = vref_none, .vreg = RC_INDEX_NONE,
                   .target = target, .target_scope = RC_INDEX_NONE, .target_def = cursor_none(),
                   .at = (cursor) {0}},
        arena);
    return pc + size;
}

RC_TEST(cfg, empty_stream_is_empty)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    cfg g = cfg_build((rc_view_zp_insn) {0}, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 0u);
    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, straight_line_is_one_block)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    // LDA / STA / RTS - no interior control flow, so one block ending in a return (no successor).
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA zp
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // STA zp
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.pc, ==, 0x2000u);
    RC_CHECK(b.first_insn, ==, 0u);
    RC_CHECK(b.num_insns, ==, 3u);
    RC_CHECK(b.succ_count, ==, 0u);            // RTS has no successor...
    RC_CHECK_FALSE(b.unknown_succ);            // ...and that is fully KNOWN (a return, not a computed exit)

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, indirect_jump_is_unknown_successor)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #..  (normal)
    //   2002  JMP (ind) (jump with a computed target the assembler cannot see -> flow=jump, target=NONE)
    // The block must NOT read as a clean dead-end (that would let liveness conclude nothing is live out and
    // shrink a range the real target still needs); it is flagged unknown_succ so the analysis stays
    // conservative. This is the "be conservative rather than silently emit broken code" guarantee.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA #
    pc = push_insn(&insns, pc, 3, zp_flow_jump, RC_INDEX_NONE, &arena);     // JMP (ind) - unknown target
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 2u);
    RC_CHECK(b.succ_count, ==, 0u);           // no successor we can place...
    RC_CHECK_TRUE(b.unknown_succ);            // ...but control DOES leave, so stay conservative

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, branch_splits_into_blocks_with_edges)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDX #.. (normal, 2 bytes)
    //   2002  BNE 2006 (branch, 2 bytes; target = 2006, fall-through = 2004)
    //   2004  LDA #.. (normal, 2 bytes)   <- fall-through block
    //   2006  RTS      (return, 1 byte)   <- taken-target block
    // Expect three blocks: [2000,2002] ending in the branch, [2004] falling through to the RTS block, [2006].
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDX #
    pc = push_insn(&insns, pc, 2, zp_flow_branch, 0x2006, &arena);         // BNE 2006
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA #
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 3u);

    // Block 0: LDX + BNE, leader 2000, two successors - fall-through (block 1) and target (block 2).
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.pc, ==, 0x2000u);
    RC_CHECK(b0.num_insns, ==, 2u);
    RC_CHECK(b0.succ_count, ==, 2u);
    RC_CHECK(cfg_succ(g, b0, 0), ==, 1u);   // not taken -> the LDA block
    RC_CHECK(cfg_succ(g, b0, 1), ==, 2u);   // taken -> the RTS block

    // Block 1: the fall-through LDA, leader 2004, falls through to block 2.
    basic_block b1 = rc_array_basic_block_get(&g.blocks, 1);
    RC_CHECK(b1.pc, ==, 0x2004u);
    RC_CHECK(b1.num_insns, ==, 1u);
    RC_CHECK(b1.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b1, 0), ==, 2u);

    // Block 2: the RTS target, leader 2006, no successor.
    basic_block b2 = rc_array_basic_block_get(&g.blocks, 2);
    RC_CHECK(b2.pc, ==, 0x2006u);
    RC_CHECK(b2.num_insns, ==, 1u);
    RC_CHECK(b2.succ_count, ==, 0u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, jump_target_leads_backward_edge)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #..  (normal)          <- loop top, a JMP target
    //   2002  JMP 2000 (jump back)
    // A backward JMP: block 0 is [2000,2002] and jumps to itself (its own leader). One block, self-edge.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA #
    pc = push_insn(&insns, pc, 3, zp_flow_jump, 0x2000, &arena);           // JMP 2000
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.pc, ==, 0x2000u);
    RC_CHECK(b.num_insns, ==, 2u);
    RC_CHECK(b.succ_count, ==, 1u);
    RC_CHECK(cfg_succ(g, b, 0), ==, 0u);   // jumps back to itself

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, call_is_in_block_not_an_edge)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  JSR 3000 (call, 3 bytes)   <- callee entry 3000 is a leader, but NOT a successor edge
    //   2003  RTS      (return)
    // The JSR falls through in-block: block 0 = [2000(JSR), 2003(RTS)], no successor (ends in RTS). The callee
    // at 3000 has no instruction recorded here, so it forms no block - it would be reached via the call graph.
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 3, zp_flow_call, 0x3000, &arena);   // JSR 3000
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS
    (void) pc;

    cfg g = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 2u);        // JSR and RTS in one block
    RC_CHECK(b.succ_count, ==, 0u);           // ends in RTS - no intraprocedural successor

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

RC_TEST(cfg, canjump_wires_declared_targets)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    rc_array_zp_insn insns = rc_array_zp_insn_make(8, &arena);

    //   2000  LDA #   (normal)  <- target A block
    //   2002  RTS
    //   2003  LDA #   (normal)  <- target B block
    //   2005  RTS
    //   2006  JMP (ind) (jump, target unknown) <- the dispatcher
    uint32_t pc = 0x2000;
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2000
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2002
    pc = push_insn(&insns, pc, 2, zp_flow_normal, RC_INDEX_NONE, &arena);   // LDA  @2003
    pc = push_insn(&insns, pc, 1, zp_flow_return, RC_INDEX_NONE, &arena);   // RTS  @2005
    pc = push_insn(&insns, pc, 3, zp_flow_jump,   RC_INDEX_NONE, &arena);   // JMP (ind) @2006
    (void) pc;

    // CANJUMP @2006 -> {2000, 2003}: two real successor edges, and the taint cleared.
    rc_array_zp_cflow cflows = rc_array_zp_cflow_make(2, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2006, .target = 0x2000, .kind = zp_cflow_canjump}, &arena);
    rc_array_zp_cflow_push(&cflows, (zp_cflow) {.site = 0x2006, .target = 0x2003, .kind = zp_cflow_canjump}, &arena);

    cfg g = cfg_build(insns.view, cflows.view, (rc_view_zp_label) {0}, &arena, scratch);
    basic_block b = rc_array_basic_block_get(&g.blocks, cfg_block_at(g, 0, 0x2006));
    RC_CHECK_FALSE(b.unknown_succ);            // CANJUMP resolved it - no taint
    RC_CHECK(b.succ_count, ==, 2u);
    RC_CHECK(cfg_succ(g, b, 0), ==, cfg_block_at(g, 0, 0x2000));
    RC_CHECK(cfg_succ(g, b, 1), ==, cfg_block_at(g, 0, 0x2003));

    // Without the annotation the same JMP stays an unknown successor with no placeable edges.
    cfg g2 = cfg_build(insns.view, (rc_view_zp_cflow) {0}, (rc_view_zp_label) {0}, &arena, scratch);
    basic_block b2 = rc_array_basic_block_get(&g2.blocks, cfg_block_at(g2, 0, 0x2006));
    RC_CHECK_TRUE(b2.unknown_succ);
    RC_CHECK(b2.succ_count, ==, 0u);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
