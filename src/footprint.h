#ifndef BARON_FOOTPRINT_H_
#define BARON_FOOTPRINT_H_

#include "cfg.h"
#include "zeropage.h"   // rc_view_zp_insn
#include "richc/arena.h"
#include "richc/bitset.h"


// A routine's zero-page footprint: every ZPAUTO vreg it touches, transitively through the routines it calls.
// This is the interprocedural core (spec's Touch(R) = In union Out union Locals union union-of-callee-Touch):
// anything a caller holds live across a JSR collides with the callee's ENTIRE footprint, because the call
// clobbers all of it. `unknown_call` means a JSR with an untrackable target was reached, so the footprint is
// incomplete - the caller must refuse rather than under-approximate. `recursive` means the call graph reached
// from this entry contains a cycle; a value live across such a call cannot be held in one static byte, so the
// caller must refuse.
typedef struct footprint {
    rc_bitset touched;        // vregs, width num_vars
    bool      unknown_call;
    bool      recursive;
} footprint;

// Compute the footprint of the routine entered at `entry_block`: the union of vregs touched by every block
// reachable from it via INTRAprocedural edges (loops are fine), plus the footprint of every routine it calls
// (each JSR target), transitively. A cycle in the CALL graph sets `recursive`; an untrackable call target (or
// a callee that itself leaves via a computed jump) sets `unknown_call`. `touched` lives in `arena`; `scratch`
// (by value, distinct from `arena`) backs the transient per-routine work sets.
footprint footprint_compute(cfg g, rc_view_zp_insn insns, uint32_t entry_block, uint32_t num_vars,
                            rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_FOOTPRINT_H_
