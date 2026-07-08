#include "cfg.h"

#include "richc/bitset.h"
#include "richc/macros.h"


enum {
    cfg_addr_space = 0x10000,   // the 6502's 64 KB - the leader bitset spans one bit per address
};


// Is `addr` the leader (entry) of a basic block? Leaders come from three sources, marked below: the very
// first instruction, every branch/jump/call TARGET, and the instruction after any branch / jump / return
// (the fall-through / next-block boundary). A JSR does NOT boundary its fall-through - a call is in-block -
// but its target IS a leader (the callee's entry).
static bool addr_is_leader(const rc_bitset *leaders, uint32_t addr)
{
    return addr < cfg_addr_space && rc_bitset_is_set(leaders, addr);
}

// Mark `addr` a leader, ignoring anything outside the 16-bit address space (a target past 0xFFFF, or the
// fall-through of the last byte of memory - neither names a real block).
static void mark_leader(rc_bitset *leaders, uint32_t addr)
{
    if (addr < cfg_addr_space) {
        rc_bitset_set(leaders, addr);
    }
}

// The block whose entry address is exactly `pc`, or RC_INDEX_NONE. Blocks are in ascending pc order, but the
// list is short (one per leader), so a linear scan is fine. Used to turn a fall-through / target ADDRESS into
// a successor block INDEX.
static uint32_t block_at_pc(rc_view_basic_block blocks, uint32_t pc)
{
    if (pc == RC_INDEX_NONE) {
        return RC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < blocks.num; i++) {
        if (rc_view_basic_block_get(blocks, i).pc == pc) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

uint32_t cfg_block_at_pc(cfg g, uint32_t pc)
{
    return block_at_pc(g.blocks.view, pc);
}

cfg cfg_build(rc_view_zp_insn insns, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena != NULL);
    cfg result = {.blocks = rc_array_basic_block_make(16, arena)};
    if (insns.num == 0) {
        return result;
    }

    // Pass 1: find the block leaders. The first instruction always leads; a control-flow instruction marks
    // its target (branch/jump/call) and, unless it is a call, the address after it (branch/jump/return end a
    // block, a call falls through in-block). We model BOTH edges of a conditional branch - the sound default
    // for liveness (a spurious edge only lengthens live ranges, never shortens them; a MISSING edge is the
    // only unsound case, which is why computed flow must taint instead). An always-taken annotation to prune
    // the dead edge is a later precision refinement, not needed for correctness.
    rc_bitset leaders = {0};
    rc_bitset_resize(&leaders, cfg_addr_space, &scratch);
    mark_leader(&leaders, rc_view_zp_insn_get(insns, 0).pc);
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn insn = rc_view_zp_insn_get(insns, i);
        uint32_t after = insn.pc + insn.size;
        switch (insn.flow) {
            case zp_flow_branch:
                mark_leader(&leaders, insn.target);   // taken edge
                mark_leader(&leaders, after);         // fall-through edge
                break;
            case zp_flow_jump:
                mark_leader(&leaders, insn.target);
                mark_leader(&leaders, after);         // boundary (block after an unconditional jump)
                break;
            case zp_flow_call:
                mark_leader(&leaders, insn.target);   // callee entry (reached via the call graph, not an edge)
                break;
            case zp_flow_return:
                mark_leader(&leaders, after);          // boundary (block after a return)
                break;
            case zp_flow_normal:
            default:
                break;
        }
    }

    // Pass 2: cut the instruction stream into blocks. A new block starts at the first instruction and at any
    // instruction whose pc is a leader; the block runs until the next such start. Leaders that fall in a gap
    // or past the end (a fall-through with no instruction there) create no block - only leaders that actually
    // carry an instruction become blocks. Assumes pc is monotonic in program order (see header).
    uint32_t current = RC_INDEX_NONE;
    for (uint32_t i = 0; i < insns.num; i++) {
        zp_insn insn = rc_view_zp_insn_get(insns, i);
        if (i == 0 || addr_is_leader(&leaders, insn.pc)) {
            current = rc_array_basic_block_push(
                &result.blocks,
                (basic_block) {.pc = insn.pc, .first_insn = i, .num_insns = 0,
                               .succ = {RC_INDEX_NONE, RC_INDEX_NONE}, .unknown_succ = false},
                arena);
        }
        rc_array_basic_block_at(&result.blocks, current)->num_insns++;
    }

    // Pass 3: wire successor edges from each block's LAST instruction's control-flow class. A branch has two
    // (fall-through + target), a jump one (target), a return none, and a call / normal terminator falls
    // through to the next block. A target that names no block (unresolved, or computed) yields NONE - a taint
    // the liveness pass will read as "unknown successor" and treat conservatively.
    for (uint32_t bi = 0; bi < result.blocks.num; bi++) {
        basic_block *block = rc_array_basic_block_at(&result.blocks, bi);
        zp_insn last = rc_view_zp_insn_get(insns, block->first_insn + block->num_insns - 1);
        uint32_t after = last.pc + last.size;
        switch (last.flow) {
            case zp_flow_branch:
                block->succ[0] = block_at_pc(result.blocks.view, after);          // not taken (in-stream)
                block->succ[1] = block_at_pc(result.blocks.view, last.target);    // taken
                if (block->succ[1] == RC_INDEX_NONE) {
                    block->unknown_succ = true;   // taken target we cannot place -> conservative
                }
                break;
            case zp_flow_jump:
                block->succ[0] = block_at_pc(result.blocks.view, last.target);
                if (block->succ[0] == RC_INDEX_NONE) {
                    block->unknown_succ = true;   // indirect/computed JMP, or a jump leaving the stream
                }
                break;
            case zp_flow_return:
                break;   // a genuine return: no successor, fully known (an RTS-dispatch would need markup)
            case zp_flow_call:
            case zp_flow_normal:
            default:
                // A call/normal terminator falls through in-stream; a fall-through with no block is the end of
                // the program (or a routine falling off its end), which is a clean end, not an unknown target.
                block->succ[0] = block_at_pc(result.blocks.view, after);
                break;
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
        (zp_insn) {.pc = pc, .size = size, .flow = flow, .rw = vref_none,
                   .vreg = RC_INDEX_NONE, .target = target, .at = (cursor) {0}},
        arena);
    return pc + size;
}

RC_TEST(cfg, empty_stream_is_empty)
{
    rc_arena arena = rc_arena_make_default();
    rc_arena scratch = rc_arena_make_default();   // distinct backing: by-value scratch must not alias arena
    cfg g = cfg_build((rc_view_zp_insn) {0}, &arena, scratch);
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

    cfg g = cfg_build(insns.view, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.pc, ==, 0x2000u);
    RC_CHECK(b.first_insn, ==, 0u);
    RC_CHECK(b.num_insns, ==, 3u);
    RC_CHECK(b.succ[0], ==, RC_INDEX_NONE);   // RTS has no successor...
    RC_CHECK(b.succ[1], ==, RC_INDEX_NONE);
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

    cfg g = cfg_build(insns.view, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 2u);
    RC_CHECK(b.succ[0], ==, RC_INDEX_NONE);   // no successor we can place...
    RC_CHECK(b.succ[1], ==, RC_INDEX_NONE);
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

    cfg g = cfg_build(insns.view, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 3u);

    // Block 0: LDX + BNE, leader 2000, two successors - fall-through (block 1) and target (block 2).
    basic_block b0 = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b0.pc, ==, 0x2000u);
    RC_CHECK(b0.num_insns, ==, 2u);
    RC_CHECK(b0.succ[0], ==, 1u);   // not taken -> the LDA block
    RC_CHECK(b0.succ[1], ==, 2u);   // taken -> the RTS block

    // Block 1: the fall-through LDA, leader 2004, falls through to block 2.
    basic_block b1 = rc_array_basic_block_get(&g.blocks, 1);
    RC_CHECK(b1.pc, ==, 0x2004u);
    RC_CHECK(b1.num_insns, ==, 1u);
    RC_CHECK(b1.succ[0], ==, 2u);
    RC_CHECK(b1.succ[1], ==, RC_INDEX_NONE);

    // Block 2: the RTS target, leader 2006, no successor.
    basic_block b2 = rc_array_basic_block_get(&g.blocks, 2);
    RC_CHECK(b2.pc, ==, 0x2006u);
    RC_CHECK(b2.num_insns, ==, 1u);
    RC_CHECK(b2.succ[0], ==, RC_INDEX_NONE);

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

    cfg g = cfg_build(insns.view, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.pc, ==, 0x2000u);
    RC_CHECK(b.num_insns, ==, 2u);
    RC_CHECK(b.succ[0], ==, 0u);   // jumps back to itself
    RC_CHECK(b.succ[1], ==, RC_INDEX_NONE);

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

    cfg g = cfg_build(insns.view, &arena, scratch);
    RC_CHECK(g.blocks.num, ==, 1u);
    basic_block b = rc_array_basic_block_get(&g.blocks, 0);
    RC_CHECK(b.num_insns, ==, 2u);        // JSR and RTS in one block
    RC_CHECK(b.succ[0], ==, RC_INDEX_NONE);   // ends in RTS - no intraprocedural successor
    RC_CHECK(b.succ[1], ==, RC_INDEX_NONE);

    rc_arena_deinit(&scratch);
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
