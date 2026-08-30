#ifndef BARON_LIVENESS_H_
#define BARON_LIVENESS_H_

#include "cfg.h"
#include "zeropage.h"
#include "richc/arena.h"
#include "richc/array/u8.h"
#include "richc/array/bitset.h"
#include "richc/bitset.h"


// How a variable relates to its routine's boundary, inferred from a single routine in isolation
// (the interprocedural passes refine across calls).
typedef enum vreg_class {
    vreg_class_unused = 0,   // touched by no recorded instruction
    vreg_class_input,        // live-in at entry (read before written): the caller supplies its value
    vreg_class_output,       // written but never read inside: the value escapes to the caller
    vreg_class_temp,         // written and read inside: invisible to callers
} vreg_class;

// The result of a liveness analysis over the global vreg id space. No separate counts: every
// container carries its own (rows per block, vregs per row), and liveness_analyze asserts they
// agree. interfere is a SPAN, not a view - the finalize guards keep injecting edges (pinning,
// call-input) into the graph after the analysis hands it over.
typedef struct liveness {
    rc_view_bitset live_in;      // [num_blocks]
    rc_view_bitset live_out;     // [num_blocks]
    rc_span_bitset interfere;    // [num_vars]: row a has bit b set iff a and b's live ranges overlap (symmetric)
    rc_view_bitset must_write;   // [num_blocks] var-level: a call entering this block definitely rewrites these
                                 //   in full on every returning path, so it KILLS them (empty off entry blocks)
    rc_view_u8     classes;      // [num_vars] vreg_class values (u8 storage; members never take enum types)
} liveness;

// Run the backward liveness fixpoint over g, build the interference graph, and classify each vreg
// (entry_block seeds the classification; RC_INDEX_NONE for none). A call is treated as a USE of its
// callees' live-in (cflows supplies the ZA_CANCALL overrides), so an argument stored by the caller
// stays live up to the JSR; vars' WIDTHS drive the partial-def rule (see zp_insn_write_kills). A
// block flagged unknown_succ contributes ALL vars to its live-out - the conservative taint that
// keeps a computed exit from shrinking a live range. Results in arena; scratch backs the transients.
liveness liveness_analyze(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, rc_view_zp_var vars,
                          uint32_t entry_block, rc_arena *arena, rc_arena scratch);

// Do vregs a and b interfere (live ranges overlap)? False if either is out of range or a == b.
bool liveness_interferes(const liveness *lv, uint32_t a, uint32_t b);

vreg_class liveness_class_of(const liveness *lv, uint32_t vreg);

// Test/inspection helpers: is vreg live on entry to / exit from block?
bool liveness_is_live_in(const liveness *lv, uint32_t block, uint32_t vreg);
bool liveness_is_live_out(const liveness *lv, uint32_t block, uint32_t vreg);

// May-read-before-write from root: the variables some real control path starting at root READS
// before any write covers the byte read - the routine's true inputs. The precise form of "live-in at
// the root": the backward liveness above smears one call site's live-after through a shared callee
// into another (context-insensitive return edges); this walk follows calls with per-callee summaries
// instead. Unknown/external arms contribute nothing. Returns a var-level bitset in arena.
rc_bitset liveness_read_before_write(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows,
                                     rc_view_zp_var vars, uint32_t root, rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_LIVENESS_H_
