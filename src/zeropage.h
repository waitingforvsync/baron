#ifndef BARON_ZEROPAGE_H_
#define BARON_ZEROPAGE_H_

#include "cursor.h"   // cursor: a ZA_AUTO's defining position, its stable identity across passes
#include "richc/arena.h"
#include "richc/bitset.h"
#include "richc/str.h"   // rc_str (a ZA_AUTO's name)
#include <stdbool.h>
#include <stdint.h>


// The zero-page auto-allocation subsystem. Today it holds only the ZA_POOL set: which of the 256
// zero-page bytes the allocator (ZA_AUTO1/ZA_AUTO2, still to come) may draw from, plus whether the feature has
// been switched on at all. A ZA_POOL directive in the source is what enables it; with no ZA_POOL the
// whole subsystem stays dormant and costs nothing. The instruction IR, liveness, interference and
// colouring will grow onto this manager as the feature is built out.
//
// The reserve set is GLOBAL - there is one physical zero page shared by all resident code, so one map
// for the whole program (per-section reuse is a later refinement). It lives in the borrowed permanent
// arena, but nothing after init allocates: the 256-bit set is stood up once and only its bits move.
// ZA_POOL re-executes every pass, so the set is cleared at the top of each pass (zeropage_reset) and
// refilled as the directive runs; after the final pass it holds the settled reservation.
// One declared zero-page variable - the seed of a "vreg" for the allocator still to come. For now it
// records just what a later allocation pass will need: the name (a view into permanent source text), the
// owning scope, the byte width (1 for ZA_AUTO1, 2 for ZA_AUTO2), and the defining cursor, which is the variable's
// identity across passes (the same statement re-walked keeps the same def).
typedef struct zp_var {
    rc_str   name;
    uint32_t scope;
    uint16_t width;   // byte count: 1 (ZA_AUTO1), 2 (ZA_AUTO2), or a generic ZA_AUTO <n> table (up to 256)
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

// How a control transfer reaches its destination. A DIRECT transfer's operand IS the destination (abs / rel).
// An indirect JMP goes THROUGH a vector: its operand names the vector cell, and the destination is whatever
// that cell holds at run time. We can still reason about it when the cell lies OUTSIDE the program - a fixed
// OS vector like (&FFFC), which by policy dispatches to external code - but a vector that is one of our own
// labels holds a value we cannot see (annotate with ZA_CANJUMP). The indexed form (JMP (table,X)) is a dispatch
// through our own table, always computed.
typedef enum zp_target_via {
    zp_target_via_direct = 0,
    zp_target_via_vector,        // JMP (addr)
    zp_target_via_table,         // JMP (addr,X)
} zp_target_via;

// One recorded instruction - the IR the CFG + liveness passes walk. EVERY instruction on the final pass is
// recorded (so pc ordering and branch targets are complete), each carrying its address + size (to find the
// fall-through / next block), its control-flow class + resolved target address (branch/jump/call; else
// RC_INDEX_NONE), and - if it touches a ZA_AUTO variable - which vreg and how (`rw`; for an indirect access
// through a pointer the pointer is always READ, whatever the instruction does to the pointed-to data).
// `vreg` is RC_INDEX_NONE for an instruction that touches no variable. Recorded on the final pass only.
typedef struct zp_insn {
    uint32_t pc;              // this instruction's address
    uint16_t size;           // its length in bytes (1 + operand bytes)
    uint8_t  flow;           // zp_flow
    uint8_t  rw;             // vref_rw, if it touches `vreg`
    uint32_t vreg;           // the ZA_AUTO it touches, or RC_INDEX_NONE - RESOLVED from (var_scope, var_def)
    uint32_t var_scope;      // scope the operand's base name was declared in (with var_def, the vreg identity)
    cursor   var_def;        // def cursor of the operand's base name, or cursor_none; with var_scope resolves vreg
    bool     var_indexed;    // the operand reaches its var by an indexed / indexed-indirect mode (var,X etc.) -
                             // outside the direct-addressing envelope, so the allocation would be unsound; a
                             // final-pass check refuses it (see zeropage_finalize)
    bool     var_indirect;   // the operand dereferences its var as a zero-page POINTER ((var),Y / (var)) - a
                             // 2-byte access, so a 1-byte ZA_AUTO1 here is refused (final-pass width check)
    uint32_t var_offset;     // the compile-time-known byte offset into the var (0 for `var`, k for `var+k`), or
                             // RC_INDEX_NONE if not statically known; a final-pass check bounds it against width
    bool     var_kill;       // a ZA_DISCARD marker, not a real instruction: the programmer's promise that the
                             // value `vreg` holds at this pc is never read again. Size 0, flow normal, rw none;
                             // the analyses treat it as a full-width kill that pins nothing, touches nothing
    uint32_t target;         // branch/jump/call target address, or RC_INDEX_NONE. Resolves WITHIN this
                             // instruction's own section only (locals @+/@-, in-section expression branches);
                             // it never crosses a section - only a named label (below) can do that.
    uint32_t target_scope;   // scope of the target LABEL, when the operand named one, else RC_INDEX_NONE
    cursor   target_def;     // def cursor of the target label, or cursor_none; with target_scope it identifies
                             // the label - and so the exact block - even where banks share the address
    uint8_t  target_via;     // zp_target_via: how the transfer reaches its destination. For an indirect jump
                             // the operand (and so target_scope/def) names the VECTOR, never the destination -
                             // the CFG must not wire an edge to the vector cell's own address
    bool     target_is_zpvar;// the target identity names a ZA_AUTO variable (resolved post-pass, like vreg):
                             // a JMP through such a vector is a cell WE own - computed flow needing ZA_CANJUMP,
                             // never the external-OS-vector exit
    uint32_t section;        // which section this instruction's bytes live in (half of a block's identity)
    cursor   at;
} zp_insn;

#define RC_ARRAY_TYPE zp_insn
#define RC_ARRAY_NAME zp_insn
#include "richc/template/array.h"

// Does this instruction's write, ON ITS OWN, fully redefine its variable - so the old value is dead just
// before it? A 6502 store writes ONE byte, so a single write covers the variable only when the variable IS
// one byte (a direct store at known offset 0). Everything else - the LSB store of a ZA_AUTO2 pointer, an
// indexed store into a table, an unknown offset - is a PARTIAL def: the bytes it does not touch flow
// through it. Treating a partial def as a kill is how a pointer's MSB, written once at init, got severed
// from its derefs and clobbered by an overlapping allocation. The liveness analysis itself tracks bytes
// (liveness.c) so an LSB+MSB store PAIR does accumulate into a kill there; this per-instruction test
// serves the finalizer's variable-granularity live-across-call sweep, where the pair conservatively does
// not (a too-live set only adds interference - sound).
static inline bool zp_insn_write_kills(zp_insn n, uint16_t width)
{
    return (n.rw & vref_write) != 0
        && !n.var_indexed
        && n.var_offset == 0
        && width == 1;
}


// A control-flow annotation: the programmer's assertion where static analysis cannot see the truth on its
// own. ZA_UNREACHABLE says control cannot fall through to its own pc (an always-taken branch's dead edge, which
// the CFG then prunes); ZA_CANCALL names the real target(s) of a JSR the analysis cannot follow (a self-modified
// or dispatched call), so the callee footprint can still be bounded; ZA_CANJUMP names the possible targets of a
// computed JMP (a jump table), which the CFG wires as real successor edges. All three are TRUSTED overrides -
// a wrong one is the single way to defeat the certainty contract - but they sit exactly where the analysis
// would otherwise refuse, turning a "cannot prove it" into the programmer's explicit "I promise it is these".
// Recorded on the final pass only, like insns.
typedef enum zp_cflow_kind {
    zp_cflow_za_unreachable = 0,   // control cannot fall through to `site`
    zp_cflow_za_cancall,           // the JSR at `site` may call `target` (overrides its literal target)
    zp_cflow_za_canjump,           // the computed JMP at `site` may jump to `target` (a jump-table edge)
} zp_cflow_kind;

typedef struct zp_cflow {
    uint32_t site;    // pc of the annotated instruction: ZA_UNREACHABLE its own pc; ZA_CANCALL/ZA_CANJUMP the JSR/JMP pc
    uint32_t target;  // a call/jump target address (ZA_CANCALL/ZA_CANJUMP); RC_INDEX_NONE for ZA_UNREACHABLE
    uint8_t  kind;    // zp_cflow_kind
    cursor   at;      // where the annotation sits, for diagnostics
} zp_cflow;

#define RC_ARRAY_TYPE zp_cflow
#define RC_ARRAY_NAME zp_cflow
#include "richc/template/array.h"


// A label marker: where in the object a label sits. It ties the label's identity - its (scope, def), the same
// pair scopes_resolve_symbol_def hands back for a reference - to its physical placement (section + address).
// The CFG uses it to turn a control-transfer target that named a label into the exact block, which is what
// lets two sections (paged banks) share an address yet resolve a `JSR bank5.entry` unambiguously - the label
// picks the section, the raw address never could. Recorded by handle_label on the final pass, feature on.
typedef struct zp_label {
    uint32_t scope;    // scope the label was defined in (matches scopes_resolve_symbol_def's .scope)
    cursor   def;      // the label's def cursor - its identity across passes
    uint32_t section;  // the section the label sits in
    uint32_t pc;       // the label's address
} zp_label;

#define RC_ARRAY_TYPE zp_label
#define RC_ARRAY_NAME zp_label
#include "richc/template/array.h"


// A declared program entry: ZA_ENTRY (a synchronous external entry - a sync reachability root) or
// ZA_INTERRUPT (an interrupt handler - an async root whose communication vars and footprint the finalize
// step pins against the rest of the program). Both are bare marker statements: they record the pc they
// stand at, so placing one just inside a routine (before or after its label - nothing is emitted, the pc
// is the same) declares that routine. Recorded on the final pass only, like the annotations above.
typedef struct zp_entry {
    uint32_t section;    // the section current at the marker statement
    uint32_t pc;         // the marked pc - the declared routine's entry address
    bool     interrupt;  // ZA_INTERRUPT vs ZA_ENTRY
    cursor   at;         // where the marker sits, for diagnostics
} zp_entry;

#define RC_ARRAY_TYPE zp_entry
#define RC_ARRAY_NAME zp_entry
#include "richc/template/array.h"


typedef struct zeropage {
    rc_arena        *arena;      // BORROWED permanent: backs the reserve bitset, var list and insn list
    rc_bitset        reserved;   // 256 bits: reserved[b] iff zero-page byte b may be auto-allocated
    rc_array_zp_var  vars;       // the declared ZA_AUTO1/ZA_AUTO2s, recorded on the final pass (see zeropage.c)
    rc_array_zp_insn insns;      // the VAR-touching instructions, recorded on the final pass
    rc_array_zp_cflow cflows;    // ZA_UNREACHABLE / ZA_CANCALL annotations, recorded on the final pass
    rc_array_zp_label labels;    // label markers (identity -> section + pc), recorded on the final pass
    rc_array_zp_entry entries;   // ZA_ENTRY / ZA_INTERRUPT markers, recorded on the final pass
    bool             enabled;    // a ZA_POOL directive has run -> the ZA_AUTO1/ZA_AUTO2 feature is active
} zeropage;

enum { zeropage_size = 256 };   // the 6502 zero page is one 256-byte page

// Stand up an empty reserve set (256 zeroed bits) with the feature off. Borrows `permanent`.
void zeropage_init(zeropage *zp, rc_arena *permanent);

// Clear for a fresh pass: no reserved bytes, feature off. Keeps the backing (only the bits move).
void zeropage_reset(zeropage *zp);

// Switch the feature on without reserving any byte (ZA_POOL with an empty list still enables it).
void zeropage_enable(zeropage *zp);

// Reserve one zero-page byte for auto-allocation (idempotent) and enable the feature. `byte` must be
// < zeropage_size (the caller range-checks the operand first).
void zeropage_reserve(zeropage *zp, uint32_t byte);

bool     zeropage_is_enabled(const zeropage *zp);
bool     zeropage_is_reserved(const zeropage *zp, uint32_t byte);   // false for byte >= zeropage_size
uint32_t zeropage_reserved_count(const zeropage *zp);

// Record a declared variable (ZA_AUTO1/ZA_AUTO2). Returns its index in the var list. Called once per ZA_AUTO on the
// final pass; the settling passes only need the placeholder symbol binding, not the registry.
uint32_t zeropage_add_var(zeropage *zp, rc_str name, uint32_t scope, uint16_t width, cursor def);

uint32_t         zeropage_var_count(const zeropage *zp);
zp_var           zeropage_var_get(const zeropage *zp, uint32_t index);
rc_view_zp_var   zeropage_vars(const zeropage *zp);          // the whole var list, for the allocator
const rc_bitset *zeropage_reserved(const zeropage *zp);      // the free-byte set the allocator draws from

// The index of the variable declared in `scope` at `def`, or RC_INDEX_NONE if that pair is not a ZA_AUTO
// declaration. A linear scan - variables are few. The (scope, def) PAIR is the identity: the def cursor alone
// collides across a macro / FOR body's instantiations (all share one def), but each instantiation runs in its
// own child scope, so the scope tells them apart.
uint32_t zeropage_find_var(const zeropage *zp, uint32_t scope, cursor def);

// Record one instruction into the IR (final pass only). Returns its index.
uint32_t zeropage_add_insn(zeropage *zp, zp_insn insn);

// Resolve every recorded insn's vreg from its var_def, now that the whole var registry is populated. Done
// once post-pass (not at record time) so a variable USED before its ZA_AUTO declaration still attributes -
// the def cursor is stable across passes, but the registry fills in source order during the final pass.
void zeropage_resolve_vregs(zeropage *zp);

uint32_t         zeropage_insn_count(const zeropage *zp);
zp_insn          zeropage_insn_get(const zeropage *zp, uint32_t index);
rc_view_zp_insn  zeropage_insns(const zeropage *zp);   // the whole insn list, for the CFG builder

// Record one control-flow annotation (final pass only). Returns its index.
uint32_t         zeropage_add_cflow(zeropage *zp, zp_cflow cf);
rc_view_zp_cflow zeropage_cflows(const zeropage *zp);   // all annotations, for the CFG + footprint passes

// Record one label marker (final pass only). Returns its index.
uint32_t          zeropage_add_label(zeropage *zp, uint32_t scope, cursor def, uint32_t section, uint32_t pc);
rc_view_zp_label  zeropage_labels(const zeropage *zp);   // all label markers, for the CFG's target resolution

// Record one entry marker (final pass only). Returns its index.
uint32_t          zeropage_add_entry(zeropage *zp, zp_entry e);
rc_view_zp_entry  zeropage_entries(const zeropage *zp);  // all entry markers, for the CFG + finalize roots


#endif // ifndef BARON_ZEROPAGE_H_
