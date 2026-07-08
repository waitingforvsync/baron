#ifndef BARON_ZEROPAGE_H_
#define BARON_ZEROPAGE_H_

#include "cursor.h"   // cursor: a ZPAUTO's defining position, its stable identity across passes
#include "richc/arena.h"
#include "richc/bitset.h"
#include "richc/str.h"   // rc_str (a ZPAUTO's name)
#include <stdbool.h>
#include <stdint.h>


// The zero-page auto-allocation subsystem. Today it holds only the ZPRESERVE set: which of the 256
// zero-page bytes the allocator (ZPAUTO1/ZPAUTO2, still to come) may draw from, plus whether the feature has
// been switched on at all. A ZPRESERVE directive in the source is what enables it; with no ZPRESERVE the
// whole subsystem stays dormant and costs nothing. The instruction IR, liveness, interference and
// colouring will grow onto this manager as the feature is built out.
//
// The reserve set is GLOBAL - there is one physical zero page shared by all resident code, so one map
// for the whole program (per-overlay reuse is a later refinement). It lives in the borrowed permanent
// arena, but nothing after init allocates: the 256-bit set is stood up once and only its bits move.
// ZPRESERVE re-executes every pass, so the set is cleared at the top of each pass (zeropage_reset) and
// refilled as the directive runs; after the final pass it holds the settled reservation.
// One declared zero-page variable - the seed of a "vreg" for the allocator still to come. For now it
// records just what a later allocation pass will need: the name (a view into permanent source text), the
// owning scope, the byte width (1 for ZPAUTO1, 2 for ZPAUTO2), and the defining cursor, which is the variable's
// identity across passes (the same statement re-walked keeps the same def).
typedef struct zp_var {
    rc_str   name;
    uint32_t scope;
    uint8_t  width;
    cursor   def;
} zp_var;

#define RC_ARRAY_TYPE zp_var
#define RC_ARRAY_NAME zp_var
#include "richc/template/array.h"


// How an instruction touches a variable, as a bitmask (an RMW instruction like INC is read|write). This is
// the raw material for the liveness analysis: a write is a def, a read is a use.
typedef enum vref_rw {
    vref_none  = 0,
    vref_read  = 1,
    vref_write = 2,
} vref_rw;

// An instruction's control-flow class, from the opcode table's op_branch/op_jump/op_call/op_return flags.
// The CFG builder splits basic blocks on these: a branch is 2-way (target + fall-through), a jump 1-way, a
// return has no successor, and a call (JSR) is IN-BLOCK - it falls through, and the callee is reached via
// the call graph, not a CFG edge.
typedef enum zp_flow {
    zp_flow_normal = 0,
    zp_flow_branch,
    zp_flow_jump,
    zp_flow_call,
    zp_flow_return,
} zp_flow;

// One recorded instruction - the IR the CFG + liveness passes walk. EVERY instruction on the final pass is
// recorded (so pc ordering and branch targets are complete), each carrying its address + size (to find the
// fall-through / next block), its control-flow class + resolved target address (branch/jump/call; else
// RC_INDEX_NONE), and - if it touches a ZPAUTO variable - which vreg and how (`rw`; for an indirect access
// through a pointer the pointer is always READ, whatever the instruction does to the pointed-to data).
// `vreg` is RC_INDEX_NONE for an instruction that touches no variable. Recorded on the final pass only.
typedef struct zp_insn {
    uint32_t pc;       // this instruction's address
    uint16_t size;     // its length in bytes (1 + operand bytes)
    uint8_t  flow;     // zp_flow
    uint8_t  rw;       // vref_rw, if it touches `vreg`
    uint32_t vreg;     // the ZPAUTO it touches, or RC_INDEX_NONE
    uint32_t target;   // branch/jump/call target address, or RC_INDEX_NONE
    cursor   at;
} zp_insn;

#define RC_ARRAY_TYPE zp_insn
#define RC_ARRAY_NAME zp_insn
#include "richc/template/array.h"


typedef struct zeropage {
    rc_arena        *arena;      // BORROWED permanent: backs the reserve bitset, var list and insn list
    rc_bitset        reserved;   // 256 bits: reserved[b] iff zero-page byte b may be auto-allocated
    rc_array_zp_var  vars;       // the declared ZPAUTO1/ZPAUTO2s, recorded on the final pass (see zeropage.c)
    rc_array_zp_insn insns;      // the VAR-touching instructions, recorded on the final pass
    bool             enabled;    // a ZPRESERVE directive has run -> the ZPAUTO1/ZPAUTO2 feature is active
} zeropage;

enum { zeropage_size = 256 };   // the 6502 zero page is one 256-byte page

// An unallocated ZPAUTO resolves to this zero-page address during the settling passes. Any value in [0,256)
// works - it only has to size `LDA var` as a 2-byte zero-page access; the real address is assigned later,
// at the allocation phase, and the operand bytes patched. A fixed sentinel keeps the binding convergent
// (constant value -> never "changed") and is honest that no allocation has happened yet.
enum { zeropage_var_placeholder = 0 };

// Stand up an empty reserve set (256 zeroed bits) with the feature off. Borrows `permanent`.
void zeropage_init(zeropage *zp, rc_arena *permanent);

// Clear for a fresh pass: no reserved bytes, feature off. Keeps the backing (only the bits move).
void zeropage_reset(zeropage *zp);

// Switch the feature on without reserving any byte (ZPRESERVE with an empty list still enables it).
void zeropage_enable(zeropage *zp);

// Reserve one zero-page byte for auto-allocation (idempotent) and enable the feature. `byte` must be
// < zeropage_size (the caller range-checks the operand first).
void zeropage_reserve(zeropage *zp, uint32_t byte);

bool     zeropage_is_enabled(const zeropage *zp);
bool     zeropage_is_reserved(const zeropage *zp, uint32_t byte);   // false for byte >= zeropage_size
uint32_t zeropage_reserved_count(const zeropage *zp);

// Record a declared variable (ZPAUTO1/ZPAUTO2). Returns its index in the var list. Called once per ZPAUTO on the
// final pass; the settling passes only need the placeholder symbol binding, not the registry.
uint32_t zeropage_add_var(zeropage *zp, rc_str name, uint32_t scope, uint8_t width, cursor def);

uint32_t zeropage_var_count(const zeropage *zp);
zp_var   zeropage_var_get(const zeropage *zp, uint32_t index);

// The index of the variable defined at `def` (its identity cursor), or RC_INDEX_NONE if `def` is not a
// ZPAUTO declaration. A linear scan - variables are few. Used to map an operand's resolved binding to a vreg.
uint32_t zeropage_find_var_by_def(const zeropage *zp, cursor def);

// Record one instruction into the IR (final pass only). Returns its index.
uint32_t zeropage_add_insn(zeropage *zp, zp_insn insn);

uint32_t         zeropage_insn_count(const zeropage *zp);
zp_insn          zeropage_insn_get(const zeropage *zp, uint32_t index);
rc_view_zp_insn  zeropage_insns(const zeropage *zp);   // the whole insn list, for the CFG builder


#endif // ifndef BARON_ZEROPAGE_H_
