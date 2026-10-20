#ifndef BARON_CFG_H_
#define BARON_CFG_H_

#include "zeropage.h"
#include "richc/arena.h"
#include "richc/array/u32.h"


// A basic block: a maximal straight-line run of instructions with a single entry and a single exit,
// indexing into the instruction list rather than owning insns. (section, pc) is its identity - paged
// banks may share a pc, and a control transfer crosses sections by a label's value (or, for a plain
// number, by landing on code nowhere else - see cfg_call_targets).
//
// unknown_succ: control may ALSO leave to an address we cannot model (an unannotated indirect jump
// through a vector WE assembled, an indexed dispatch, an unresolved target), so the succ slice is
// incomplete and liveness must go conservative. A constant destination off the stream is NOT this -
// that is a clean external exit (cfg_target_is_external) - nor is a plain RTS/RTI (no successor,
// fully known). A self-modified transfer is invisible here altogether: the ZA_ annotations supply
// what we cannot recover, and unannotated it remains a trusted precondition.
typedef struct basic_block {
    uint32_t section;        // with pc, the block's identity
    uint32_t pc;             // leader address (this block's entry), within its section
    uint32_t first_insn;     // index of the first instruction (into the insn list)
    uint32_t num_insns;      // instruction count
    uint32_t succ_first;     // index of this block's first successor in cfg.succs
    uint32_t succ_count;     // number of successor block indices
    bool     unknown_succ;   // control may also leave to an address we cannot model (see above)
} basic_block;

#define RC_ARRAY_TYPE basic_block
#define RC_ARRAY_NAME basic_block
#include "richc/template/array.h"


// The control-flow graph over one instruction stream. Intraprocedural: a JSR (call) is an in-block
// instruction, NOT an edge - the callee is reached through the call graph - and an RTS (return) ends a block
// with no successor. So the "routines" fall out as the sets of blocks reachable from an entry without
// crossing a call/return; a program with several subroutines yields several disconnected clusters here.
typedef struct cfg {
    rc_view_basic_block  blocks;   // built in full by cfg_build; nothing grows or mutates it after
    rc_view_u32          succs;    // the shared successor pool; each block owns a contiguous slice of it
    rc_view_zp_label     labels;   // retained so a label's value resolves to its block on demand (cfg_call_targets)
} cfg;

// Build the CFG for insns (recorded in program order, sections interleaving). labels resolve a
// labelled target across sections; entries' addresses are leader-marked so a mid-run ZA_ENTRY starts
// its own block; cflows applies the annotations (ZA_UNREACHABLE prunes a dead fall-through,
// ZA_CANJUMP replaces a computed transfer's targets, ZA_RETURN makes one a return to our caller,
// ZA_RETURNTO reroutes a call's continuation). Blocks + successors live in arena; scratch backs the
// transient leader set. Assumes pc is monotonic within a section run (the section-change cut keeps it so).
cfg cfg_build(rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_label labels,
              rc_view_zp_entry entries, rc_arena *arena, rc_arena scratch);

// The index of the block whose entry is exactly (section, pc), or RC_INDEX_NONE. Maps a fall-through /
// in-section target LOCATION to the block it enters (a linear scan - blocks are few).
uint32_t cfg_block_at(cfg g, uint32_t section, uint32_t pc);

// Does n's control transfer leave the assembled program for external code (an OS or ROM entry)? True for a
// direct branch/jump/call whose destination lands on no assembled code (a constant off the stream, or a label
// on code we did not assemble, like an INCBIN'd player), and for an indirect JMP through a vector that is
// not one of our own labels (a fixed OS vector). It separates "went somewhere we did not assemble" (benign -
// external code touches no ZA_AUTO, since ZA_POOL names bytes nothing outside the program uses) from "went
// somewhere of ours" and "went somewhere we cannot pin down" (the conservative taint / unknown-call case).
bool cfg_target_is_external(cfg g, zp_insn n);

// The section a declared annotation target (ZA_CANJUMP / ZA_CANCALL / ZA_RETURNTO) lands in: a
// label's value is in the label's own section, as a literal operand's is; a plain number stays in the
// site's section.
uint32_t cfg_cflow_target_section(cfg g, zp_cflow cf);

// The i-th successor block index of b (i < b.succ_count). Reads the shared successor pool.
uint32_t cfg_succ(cfg g, basic_block b, uint32_t i);

// The callee entry blocks a call site can reach - the single call-target policy, shared by the
// footprint walk and the callee-input liveness injection. A ZA_CANCALL at the site overrides the
// literal target with the declared set; a declared address with no block is an external arm, which
// returns having touched nothing (so a must-write intersection over the arms is empty). Unannotated,
// the operand decides: a label's value lands exactly where the label sits; a plain number on its own
// section's code if there is any at that address, else on every section's (a bare JSR &8003 cannot
// say which bank); nothing found is an external call.
typedef struct call_targets {
    rc_view_u32  blocks;    // in-program callee entry block indices (zero-init = no arms)
    bool         unknown;   // an untrackable (computed, unannotated) arm
    bool         external;  // an arm that leaves the program (JSR &FFEE, or an external ZA_CANCALL target)
} call_targets;

#define RC_ARRAY_TYPE call_targets
#define RC_ARRAY_NAME call_targets
#include "richc/template/array.h"

call_targets cfg_call_targets(cfg g, rc_view_zp_cflow cflows, zp_insn n, rc_arena *arena);


#endif // ifndef BARON_CFG_H_
