#ifndef BARON_CFG_H_
#define BARON_CFG_H_

#include "zeropage.h"   // zp_insn, rc_view_zp_insn, zp_flow, rc_view_zp_cflow
#include "richc/arena.h"
#include "richc/array/u32.h"   // rc_array_u32 - the successor pool


// A basic block: a maximal straight-line run of instructions with a single entry (its first instruction) and
// a single exit (its last). It indexes into the instruction list rather than owning insns. Successors are a
// contiguous slice [succ_first, succ_first + succ_count) of the CFG's shared `succs` pool - a variable count,
// so a CANJUMP jump table with many targets is representable, not just the one/two of a jump/branch.
typedef struct basic_block {
    uint32_t section;        // the section this block lives in - with pc, the block's identity. Two sections
                             // (paged banks) may share a pc, so pc ALONE does not name a block: (section, pc)
                             // does. Fall-through and in-section branches stay within one section; only a
                             // control transfer that named a label crosses into another.
    uint32_t pc;             // leader address (this block's entry), within its section
    uint32_t first_insn;     // index of the first instruction (into the insn list)
    uint32_t num_insns;      // instruction count
    uint32_t succ_first;     // index of this block's first successor in cfg.succs
    uint32_t succ_count;     // number of successor block indices
    bool     unknown_succ;   // control may ALSO leave to an address we cannot model: an indirect / computed
                             // JMP with no CANJUMP, or a jump/branch target outside the recorded stream. The
                             // succ slice then lists only the successors we CAN place, and liveness must treat
                             // live-out conservatively (everything live) rather than trust the slice as
                             // complete. This is NOT a plain RTS/RTI return - a return has NO successor and is
                             // fully known. (An RTS-dispatch masquerading as a return, and a self-modified JSR,
                             // cannot be seen here; CANCALL/CANJUMP annotations supply the targets we cannot
                             // recover. Until then those are unsound-if-unannotated preconditions.)
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
    rc_array_u32         succs;    // the shared successor pool; each block owns a contiguous slice of it
    rc_view_zp_label     labels;   // the label markers, retained so a target that named a label resolves to
                                   // its block on demand (cfg_target_block) - the callee footprint walk needs
                                   // it after the graph is built, not just during construction
} cfg;

// Build the CFG for `insns` (recorded in program / pc order, sections interleaving). `cflows` supplies the
// control-flow annotations (UNREACHABLE prunes a branch's dead fall-through; CANJUMP wires a computed JMP's
// declared targets); `labels` maps each label's identity to its (section, pc) so a control transfer that named
// a label resolves across sections. Blocks + successors live in `arena`; `scratch` (by value) backs the
// transient leader set. An empty instruction list gives an empty CFG. Assumes pc is monotonic WITHIN a section
// run (the block cut at every section change keeps it so, even as sections interleave in the stream).
cfg cfg_build(rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_label labels,
              rc_arena *arena, rc_arena scratch);

// The index of the block whose entry is exactly (`section`, `pc`), or RC_INDEX_NONE. Maps a fall-through /
// in-section target LOCATION to the block it enters (a linear scan - blocks are few).
uint32_t cfg_block_at(cfg g, uint32_t section, uint32_t pc);

// The block a control-transfer instruction `n` targets, or RC_INDEX_NONE. A named-label target resolves
// through the markers to its own section's block (crossing sections); a bare numeric target resolves within
// `n`'s own section only; a computed / unregistered target resolves to nothing. This is the one place that
// turns a target into a block, shared by the CFG's edge wiring and the callee-footprint walk.
uint32_t cfg_target_block(cfg g, zp_insn n);

// The i-th successor block index of `b` (i < b.succ_count). Reads the shared successor pool.
uint32_t cfg_succ(cfg g, basic_block b, uint32_t i);


#endif // ifndef BARON_CFG_H_
