#ifndef BARON_FOOTPRINT_H_
#define BARON_FOOTPRINT_H_

#include "cfg.h"
#include "zeropage.h"   // rc_view_zp_insn
#include "richc/arena.h"
#include "richc/bitset.h"


// A routine's zero-page footprint: every ZA_AUTO vreg it touches, transitively through the routines it calls.
// This is the interprocedural core (spec's Touch(R) = In union Out union Locals union union-of-callee-Touch):
// anything a caller holds live across a JSR collides with the callee's ENTIRE footprint, because the call
// clobbers all of it. `unknown_call` means a JSR with an untrackable target was reached, so the footprint is
// incomplete - the caller must refuse rather than under-approximate. `recursive` means the call graph reached
// from this entry contains a cycle. Recursion alone is NOT fatal: a value merely read or read-modified across
// it (a shared accumulator, DEC/INC) rides safely on one static byte. What CANNOT is a value the cycle FRESHLY
// writes (a write-only def, in `killed`) and carries live across the call - that wants a distinct byte per
// level. So the caller refuses only when a live-across variable is also in `killed` of a recursive footprint.
typedef struct footprint {
    rc_bitset touched;        // vregs, width num_vars
    rc_bitset killed;         // vregs given a write-only def (a fresh value, not an accumulation) somewhere within
    bool      unknown_call;
    bool      recursive;
} footprint;

// Compute the footprint of the routine entered at `entry_block`: the union of vregs touched by every block
// reachable from it via INTRAprocedural edges (loops are fine), plus the footprint of every routine it calls
// (each JSR target), transitively. A cycle in the CALL graph sets `recursive`; an untrackable call target (or
// a callee that itself leaves via a computed jump) sets `unknown_call`. A ZA_CANCALL annotation in `cflows`
// overrides a JSR's literal target with the declared set (so a self-modified / dispatched call can still be
// bounded). `touched` lives in `arena`; `scratch` (by value, distinct from `arena`) backs the transient
// per-routine work sets.
footprint footprint_compute(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, uint32_t entry_block,
                            uint32_t num_vars, rc_arena *arena, rc_arena scratch);

// The footprint of everything a single call site `call` may reach: the union over its callee(s). A ZA_CANCALL
// annotation in `cflows` naming `call.pc` overrides the literal target with the declared set; otherwise the
// literal target (`call.target`) is used. A target resolving to no block sets `unknown_call`. This is what a
// caller needs to test a value held live across the call - the callee clobbers its whole footprint.
footprint footprint_of_call(cfg g, rc_view_zp_insn insns, rc_view_zp_cflow cflows, zp_insn call,
                            uint32_t num_vars, rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_FOOTPRINT_H_
