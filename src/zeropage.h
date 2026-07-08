#ifndef BARON_ZEROPAGE_H_
#define BARON_ZEROPAGE_H_

#include "richc/arena.h"
#include "richc/bitset.h"
#include <stdbool.h>
#include <stdint.h>


// The zero-page auto-allocation subsystem. Today it holds only the RESERVE set: which of the 256
// zero-page bytes the allocator (VAR1/VAR2, still to come) may draw from, plus whether the feature has
// been switched on at all. A RESERVE directive in the source is what enables it; with no RESERVE the
// whole subsystem stays dormant and costs nothing. The instruction IR, liveness, interference and
// colouring will grow onto this manager as the feature is built out.
//
// The reserve set is GLOBAL - there is one physical zero page shared by all resident code, so one map
// for the whole program (per-overlay reuse is a later refinement). It lives in the borrowed permanent
// arena, but nothing after init allocates: the 256-bit set is stood up once and only its bits move.
// RESERVE re-executes every pass, so the set is cleared at the top of each pass (zeropage_reset) and
// refilled as the directive runs; after the final pass it holds the settled reservation.
typedef struct zeropage {
    rc_arena *arena;      // BORROWED permanent: backs the reserve bitset (allocated once, at init)
    rc_bitset reserved;   // 256 bits: reserved[b] iff zero-page byte b may be auto-allocated
    bool      enabled;    // a RESERVE directive has run -> the VAR1/VAR2 feature is active
} zeropage;

enum { zeropage_size = 256 };   // the 6502 zero page is one 256-byte page

// Stand up an empty reserve set (256 zeroed bits) with the feature off. Borrows `permanent`.
void zeropage_init(zeropage *zp, rc_arena *permanent);

// Clear for a fresh pass: no reserved bytes, feature off. Keeps the backing (only the bits move).
void zeropage_reset(zeropage *zp);

// Switch the feature on without reserving any byte (RESERVE with an empty list still enables it).
void zeropage_enable(zeropage *zp);

// Reserve one zero-page byte for auto-allocation (idempotent) and enable the feature. `byte` must be
// < zeropage_size (the caller range-checks the operand first).
void zeropage_reserve(zeropage *zp, uint32_t byte);

bool     zeropage_is_enabled(const zeropage *zp);
bool     zeropage_is_reserved(const zeropage *zp, uint32_t byte);   // false for byte >= zeropage_size
uint32_t zeropage_reserved_count(const zeropage *zp);


#endif // ifndef BARON_ZEROPAGE_H_
