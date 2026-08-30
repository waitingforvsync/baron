#ifndef BARON_CFG_H_
#define BARON_CFG_H_

#include "zeropage.h"
#include "richc/arena.h"
#include "richc/array/u32.h"


// A basic block: a maximal straight-line run of instructions with a single entry and a single exit,
// indexing into the instruction list rather than owning insns. (section, pc) is its identity - paged
// banks may share a pc, and only a control transfer that names a label crosses sections.
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
    rc_view_zp_label     labels;   // retained so a labelled target resolves to its block on demand (cfg_target_block)
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

// The block a control-transfer instruction n targets, or RC_INDEX_NONE. A named-label target resolves
// through the markers to its own section's block (crossing sections); a bare numeric target resolves within
// n's own section only; a computed / unregistered target resolves to nothing. This is the one place that
// turns a target into a block, shared by the CFG's edge wiring and the callee-footprint walk.
uint32_t cfg_target_block(cfg g, zp_insn n);

// Does n's control transfer leave the assembled program for external code (an OS or ROM entry)? True for a
// direct branch/jump/call whose constant destination matches no assembled code, and for an indirect JMP
// through a vector that is not one of our own labels (a fixed OS vector). Meaningful once cfg_target_block
// has come back RC_INDEX_NONE: it separates "went somewhere we did not assemble" (benign - external code
// touches no ZA_AUTO, since ZA_POOL names bytes nothing outside the program uses) from "went somewhere we
// cannot pin down" (the conservative taint / unknown-call case).
bool cfg_target_is_external(cfg g, zp_insn n);

// The i-th successor block index of b (i < b.succ_count). Reads the shared successor pool.
uint32_t cfg_succ(cfg g, basic_block b, uint32_t i);

// The callee entry blocks a call site can reach - the single call-target policy, shared by the
// footprint walk and the callee-input liveness injection. A ZA_CANCALL at the site overrides the
// literal target with the declared set; a declared address with no block is an external arm, which
// returns having touched nothing (so a must-write intersection over the arms is empty).
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
