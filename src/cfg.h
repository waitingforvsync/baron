#ifndef BARON_CFG_H_
#define BARON_CFG_H_

#include "zeropage.h"   // zp_insn, rc_view_zp_insn, zp_flow
#include "richc/arena.h"


// A basic block: a maximal straight-line run of instructions with a single entry (its first instruction)
// and a single exit (its last). It indexes into the instruction list rather than owning insns. `succ` names
// the successor blocks - fall-through and/or branch target - with RC_INDEX_NONE terminating the (<=2) list.
typedef struct basic_block {
    uint32_t pc;             // leader address (this block's entry)
    uint32_t first_insn;     // index of the first instruction (into the insn list)
    uint32_t num_insns;      // instruction count
    uint32_t succ[2];        // KNOWN successor block indices, RC_INDEX_NONE-terminated
    bool     unknown_succ;   // control may also leave to an address we cannot model: an indirect / computed
                             // JMP, or a jump/branch target outside the recorded stream. succ[] then lists
                             // only the successors we CAN place, and a later liveness pass must treat live-out
                             // conservatively (everything live) rather than trust succ[] as complete. This is
                             // NOT a plain RTS/RTI return - a return has NO successor and is fully known.
                             // (An RTS-dispatch masquerading as a return, and a self-modified JSR/JMP, cannot
                             // be seen here at all; future CANCALL/CANJUMP annotations will supply their
                             // targets. Until then those are unsound-if-unannotated preconditions, not taints
                             // this builder can detect.)
} basic_block;

#define RC_ARRAY_TYPE basic_block
#define RC_ARRAY_NAME basic_block
#include "richc/template/array.h"


// The control-flow graph over one instruction stream. Intraprocedural: a JSR (call) is an in-block
// instruction, NOT an edge - the callee is reached through the call graph - and an RTS (return) ends a block
// with no successor. So the "routines" fall out as the sets of blocks reachable from an entry without
// crossing a call/return; a program with several subroutines yields several disconnected clusters here.
typedef struct cfg {
    rc_array_basic_block blocks;
} cfg;

// Build the CFG for `insns` (recorded in program / pc order). Blocks live in `arena`; `scratch` (by value)
// backs the transient leader set. An empty instruction list gives an empty CFG. Assumes pc is monotonic in
// program order (true without an ORG that rewinds the pointer mid-stream - a later concern).
cfg cfg_build(rc_view_zp_insn insns, rc_arena *arena, rc_arena scratch);

// The index of the block whose entry address is exactly `pc`, or RC_INDEX_NONE. Maps a call/jump TARGET
// address to the block it enters (a linear scan - blocks are few).
uint32_t cfg_block_at_pc(cfg g, uint32_t pc);


#endif // ifndef BARON_CFG_H_
