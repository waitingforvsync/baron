#ifndef BARON_ZPALLOC_H_
#define BARON_ZPALLOC_H_

#include "liveness.h"
#include "zeropage.h"   // rc_view_zp_var
#include "richc/arena.h"
#include "richc/bitset.h"


// The result of colouring the interference graph into the reserved zero-page bytes: each vreg's assigned base
// byte, or RC_INDEX_NONE if it could not be placed (a spill - more simultaneously-live variables than reserved
// bytes). `any_spilled` is the quick "did anything fail" flag.
typedef struct zp_coloring {
    uint32_t  num_vars;
    uint32_t *base;          // [num_vars] assigned ZP base byte, or RC_INDEX_NONE (spilled)
    bool      any_spilled;
} zp_coloring;

// Width-aware graph colouring: assign each variable a base byte drawn from `reserved` such that two variables
// share bytes only when their live ranges are disjoint (they do not interfere), and a width-2 variable gets
// two CONSECUTIVE reserved bytes. A variable the analysis never saw touched (class unused) has no liveness to
// reason about, so it is never shared - it gets its own byte (certainty over tightness). First-fit-decreasing:
// the wider variables are placed first, each at the lowest feasible base. Results live in `arena`; `scratch`
// (by value) is reserved for future ordering needs. vreg id == index into `vars`.
zp_coloring zp_color(const liveness *lv, rc_view_zp_var vars, const rc_bitset *reserved,
                     rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_ZPALLOC_H_
