#ifndef BARON_LIVENESS_H_
#define BARON_LIVENESS_H_

#include "cfg.h"
#include "zeropage.h"   // rc_view_zp_insn, vref_rw
#include "richc/arena.h"
#include "richc/bitset.h"


// How a variable relates to its routine's boundary, inferred from liveness. This is a Stage C heuristic
// derived from a single routine in isolation; the interprocedural pass (C3) refines OUTPUT precisely from the
// real caller liveness across each call site. The three kinds are the spec's In / Out / Local:
//   input  - live-in at the routine entry (read before it is written), so the caller must supply its value.
//   output - written inside but never read inside: the value's only consumer is outside -> it escapes.
//   temp   - written and read inside: born and consumed within the routine, invisible to callers.
typedef enum vreg_class {
    vreg_class_unused = 0,   // touched by no recorded instruction
    vreg_class_input,
    vreg_class_output,
    vreg_class_temp,
} vreg_class;

// The result of a liveness analysis over the global vreg id space [0, num_vars): the per-block live-in/out
// sets from the backward fixpoint, the interference graph (one adjacency bitset row per vreg - two vregs
// interfere iff their live ranges overlap at some program point), and each vreg's boundary classification.
typedef struct liveness {
    uint32_t    num_vars;
    uint32_t    num_blocks;
    rc_bitset  *live_in;      // [num_blocks]
    rc_bitset  *live_out;     // [num_blocks]
    rc_bitset  *interfere;    // [num_vars]: interfere[a] has bit b set iff vregs a and b overlap (symmetric)
    vreg_class *classes;      // [num_vars]
} liveness;

// Run the backward liveness fixpoint over `g`, build the per-instruction interference graph, and classify
// each vreg. `insns` is the instruction stream the CFG indexes; `cflows` supplies the CANCALL overrides for
// call-target resolution - a call is treated as a USE of its callees' live-in (their inputs), so an argument
// stored by the caller stays live up to the JSR; `vars` is the variable registry - vreg ids index into it,
// its length bounds the id space, and the WIDTHS drive the partial-def rule (a write kills a live range only
// when it covers the whole variable; see zp_insn_write_kills). `entry_block` is the routine's entry (block 0
// for a whole-stream analysis). A block flagged unknown_succ contributes ALL vars to its live-out - the
// conservative taint that keeps a computed/indirect exit from silently shrinking a live range. Results live
// in `arena`; `scratch` (by value) backs the transient state.
liveness liveness_analyze(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_var vars,
                          uint32_t entry_block, rc_arena *arena, rc_arena scratch);

// Do vregs `a` and `b` interfere (live ranges overlap)? False if either is out of range or a == b.
bool liveness_interferes(const liveness *lv, uint32_t a, uint32_t b);

vreg_class liveness_class_of(const liveness *lv, uint32_t vreg);

// Test/inspection helpers: is `vreg` live on entry to / exit from `block`?
bool liveness_is_live_in(const liveness *lv, uint32_t block, uint32_t vreg);
bool liveness_is_live_out(const liveness *lv, uint32_t block, uint32_t vreg);


#endif // ifndef BARON_LIVENESS_H_
