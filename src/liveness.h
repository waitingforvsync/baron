#ifndef BARON_LIVENESS_H_
#define BARON_LIVENESS_H_

#include "cfg.h"
#include "zeropage.h"   // rc_view_zp_insn, vref_rw
#include "richc/arena.h"
#include "richc/array/u8.h"   // rc_view_u8: the classes row (u8 storage for vreg_class values)
#include "richc/bitset.h"

// Bitset rows - the shape every dataflow result takes (one row per block, or per vreg). The arena owns
// the storage; a fixed-count allocation travels as a SPAN (mutable rows, no growth), and a result only
// ever read travels as a VIEW.
#define RC_ARRAY_TYPE rc_bitset
#define RC_ARRAY_NAME bitset
#include "richc/template/array.h"


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
    // No separate counts: every container carries its own - rows per block (live_in.num etc.), vregs per
    // row (each row's bit count, == interfere.num == classes.num). liveness_analyze asserts they agree.
    rc_view_bitset live_in;      // [num_blocks]; a view - the analysis result is read-only
    rc_view_bitset live_out;     // [num_blocks]
    rc_span_bitset interfere;    // [num_vars]: row a has bit b set iff vregs a and b overlap (symmetric).
                                 // A SPAN, not a view: the finalize guards keep injecting edges (pinning,
                                 // call-input edges) into the graph after the analysis hands it over
    rc_view_bitset must_write;   // [num_blocks] var-level: for a call-target entry block, the variables the
                                 // routine entered there definitely rewrites IN FULL on every returning path
                                 // (empty for non-entry blocks). A call to it KILLS these - the caller's
                                 // pre-call values are dead, so a result's range starts at its call
    rc_view_u8     classes;      // [num_vars] vreg_class values (u8 storage; members never take enum types)
} liveness;

// Run the backward liveness fixpoint over `g`, build the per-instruction interference graph, and classify
// each vreg. `insns` is the instruction stream the CFG indexes; `cflows` supplies the ZA_CANCALL overrides for
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

// May-read-before-write from `root`: the variables some real control path starting at `root` READS before
// any write covers the byte read - the routine's true inputs. This is the precise form of "live-in at the
// root": the backward liveness above answers the same question only up to its context-INsensitive return
// edges, which smear one call site's live-after through a shared callee into another call site (a helper
// called both from a pre-init stretch and from the main loop makes everything the loop keeps live look
// live-in at the entry). This walk follows calls with per-callee summaries instead - a callee's reads
// count only where the caller has not already definitely written the bytes, and its must-writes extend
// the caller's written set - so that path does not exist here. Unknown/external call arms contribute
// nothing (no false alarms; Guard 1 refuses computed flow anyway). Returns a var-level bitset (vars.num
// bits) allocated in `arena`.
rc_bitset liveness_read_before_write(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows,
                                     rc_view_zp_var vars, uint32_t root, rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_LIVENESS_H_
