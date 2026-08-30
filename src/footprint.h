#ifndef BARON_FOOTPRINT_H_
#define BARON_FOOTPRINT_H_

#include "cfg.h"
#include "zeropage.h"
#include "richc/arena.h"
#include "richc/bitset.h"


// A routine's zero-page footprint: every ZA_AUTO vreg it touches, transitively through the routines
// it calls. Anything a caller holds live across a JSR collides with the callee's ENTIRE footprint.
// unknown_call means an untrackable target made the footprint incomplete - the caller must refuse
// rather than under-approximate. Recursion alone is not fatal (a shared accumulator rides safely on
// one static byte); what cannot is a value the cycle FRESHLY writes (in killed) held live across the
// call - a per-level value one static byte cannot serve. The caller refuses exactly that combination.
typedef struct footprint {
    rc_bitset touched;        // vregs, width num_vars
    rc_bitset killed;         // vregs given a write-only def (a fresh value, not an accumulation) somewhere within
    bool      unknown_call;
    bool      recursive;
} footprint;

// Compute the footprint of the routine entered at entry_block: the vregs touched by every block
// reachable via INTRAprocedural edges, plus the footprint of every routine it calls, transitively.
// A cycle in the CALL graph sets recursive; an untrackable call target (or a callee leaving via a
// computed jump) sets unknown_call. Results in arena; scratch backs the per-routine work sets.
footprint footprint_compute(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, uint32_t entry_block,
                            uint32_t num_vars, rc_arena *arena, rc_arena scratch);

// The footprint of everything a single call site may reach: the union over its callee arms (a
// ZA_CANCALL naming call.pc overrides the literal target). This is what a caller tests a value held
// live across the call against - the callee clobbers its whole footprint.
footprint footprint_of_call(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, zp_insn call,
                            uint32_t num_vars, rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_FOOTPRINT_H_
