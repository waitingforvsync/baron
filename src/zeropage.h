#ifndef BARON_ZEROPAGE_H_
#define BARON_ZEROPAGE_H_

#include "cursor.h"
#include "richc/arena.h"
#include "richc/bitset.h"
#include "richc/str.h"
#include <stdbool.h>
#include <stdint.h>


// The zero-page auto-allocation subsystem: the ZA_POOL byte set the allocator may draw from, plus
// the IR the analyses walk - declared variables, recorded instructions, control-flow annotations,
// label and entry markers. A ZA_POOL directive in the source enables the feature; with no ZA_POOL
// the whole subsystem stays dormant and costs nothing.
//
// The pool set is GLOBAL - there is one physical zero page shared by all resident code, so one map
// for the whole program. Everything lives in the borrowed permanent arena. ZA_POOL re-executes every
// pass, so the state is cleared at the top of each pass (zeropage_reset) and refilled as the source
// runs; after the final pass it holds the settled program.

// One declared zero-page variable - a "vreg" for the allocator: the name (a view into permanent
// source text), the owning scope, the byte width, and the defining cursor, which is the variable's
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
// the call graph, not a CFG edge. A skip is the BITZP/BITABS trick - a lone BIT opcode whose operand fetch
// swallows the next skip_bytes bytes: exactly one successor, the resume address pc + size + skip_bytes.
// The swallowed record at pc+1 is entered only by an explicit branch, never from the skip itself.
typedef enum zp_flow {
    zp_flow_normal = 0,
    zp_flow_branch,
    zp_flow_jump,
    zp_flow_call,
    zp_flow_return,
    zp_flow_skip,
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

// One recorded instruction - the IR the CFG + liveness passes walk. EVERY instruction on the final
// pass is recorded (so pc ordering and branch targets are complete), each carrying its address +
// size, control-flow class + target, and - if it touches a ZA_AUTO variable - which vreg and how.
//
// Identities resolve LATE: vreg comes from the (var_scope, var_def) pair once the whole registry is
// populated (zeropage_resolve_vregs), so a use before its declaration still attributes; a target's
// (target_scope, target_def) likewise names the exact label - and so the exact block - even where
// paged banks share an address. For an INDIRECT jump the operand names the VECTOR, never the
// destination, so the CFG must not wire an edge to the vector cell's own address. A raw target
// address resolves within its own section only; crossing a section takes a named label.
typedef struct zp_insn {
    uint32_t pc;               // this instruction's address
    uint16_t size;             // its length in bytes (1 + operand bytes)
    uint8_t  flow;             // zp_flow
    uint8_t  skip_bytes;       // zp_flow_skip only: run-time bytes the BIT swallows (BITZP 1, BITABS 2); else 0
    uint8_t  rw;               // vref_rw, if it touches vreg (an indirect access READS its pointer, whatever else it does)
    uint32_t vreg;             // the ZA_AUTO it touches, or RC_INDEX_NONE (resolved post-pass)
    uint32_t var_scope;        // scope the operand's base name was declared in (with var_def, the vreg identity)
    cursor   var_def;          // def cursor of the operand's base name, or cursor_none
    bool     var_indexed;      // reached by an indexed / indexed-indirect mode: outside the envelope (finalize warns)
    bool     var_indirect;     // dereferenced as a zero-page POINTER ((var),Y / (var)): ZA_AUTO1 here is refused
    uint32_t var_offset;       // compile-time byte offset into the var (k for var+k), or RC_INDEX_NONE if unknown
    bool     var_kill;         // a ZA_DISCARD marker, not a real instruction: a full-width kill that pins nothing
    uint32_t target;           // branch/jump/call target address (same section only), or RC_INDEX_NONE
    uint32_t target_scope;     // scope of the target LABEL, when the operand named one, else RC_INDEX_NONE
    cursor   target_def;       // def cursor of the target label, or cursor_none; with target_scope, its identity
    uint8_t  target_via;       // zp_target_via: how the transfer reaches its destination
    bool     target_is_zpvar;  // the target names a ZA_AUTO vector WE own: computed flow, never an external OS vector
    uint32_t section;          // which section this instruction's bytes live in (half of a block's identity)
    cursor   at;
} zp_insn;

#define RC_ARRAY_TYPE zp_insn
#define RC_ARRAY_NAME zp_insn
#include "richc/template/array.h"

// Does this instruction's write, ON ITS OWN, fully redefine its variable - so the old value is dead
// just before it? A 6502 store writes ONE byte, so a single write covers the variable only when the
// variable IS one byte (a direct store at known offset 0); everything else is a PARTIAL def whose
// untouched bytes flow through it. The byte-level liveness (liveness.c) does let an LSB+MSB store
// PAIR accumulate into a kill; this per-instruction test serves the finalizer's variable-granularity
// live-across-call sweep, where the pair conservatively does not (extra interference only - sound).
static inline bool zp_insn_write_kills(zp_insn n, uint16_t width)
{
    return (n.rw & vref_write) != 0
        && !n.var_indexed
        && n.var_offset == 0
        && width == 1;
}


// A control-flow annotation: the programmer's assertion where static analysis cannot see the truth
// on its own. All are TRUSTED overrides - a wrong one is the single way to defeat the certainty
// contract - but they sit exactly where the analysis would otherwise refuse, turning a "cannot prove
// it" into the programmer's explicit "I promise it is these". Recorded on the final pass only.
typedef enum zp_cflow_kind {
    zp_cflow_za_unreachable = 0,   // control cannot fall through to site
    zp_cflow_za_cancall,           // the JSR at site may call target (overrides its literal target)
    zp_cflow_za_canjump,           // the computed JMP/branch at site may go to target (a jump-table edge)
    zp_cflow_za_return,            // the jump/branch at site hands control back to our caller (no target)
    zp_cflow_za_returnto,          // the call at site resumes at target, not at the next instruction
} zp_cflow_kind;

typedef struct zp_cflow {
    uint32_t site;    // pc of the annotated instruction: ZA_UNREACHABLE its own pc; the others the JSR/JMP/branch pc
    uint32_t target;  // a target address (ZA_CANCALL/ZA_CANJUMP/ZA_RETURNTO); RC_INDEX_NONE for the bare markers
    uint8_t  kind;    // zp_cflow_kind
    cursor   at;      // where the annotation sits, for diagnostics
} zp_cflow;

#define RC_ARRAY_TYPE zp_cflow
#define RC_ARRAY_NAME zp_cflow
#include "richc/template/array.h"


// A label marker: where in the object a label sits. It ties the label's identity - its (scope, def), the same
// pair scopes_resolve_symbol_def hands back for a reference - to its physical placement (section + address).
// The CFG uses it to turn a control-transfer target that named a label into the exact block, which is what
// lets two sections (paged banks) share an address yet resolve a JSR bank5.entry unambiguously - the label
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
    rc_array_zp_cflow cflows;    // control-flow annotations (ZA_UNREACHABLE etc.), recorded on the final pass
    rc_array_zp_label labels;    // label markers (identity -> section + pc), recorded on the final pass
    rc_array_zp_entry entries;   // ZA_ENTRY / ZA_INTERRUPT markers, recorded on the final pass
    bool             enabled;    // a ZA_POOL directive has run -> the ZA_AUTO1/ZA_AUTO2 feature is active
} zeropage;

enum { zeropage_size = 256 };   // the 6502 zero page is one 256-byte page

// Stand up an empty reserve set (256 zeroed bits) with the feature off. Borrows permanent.
void zeropage_init(zeropage *zp, rc_arena *permanent);

// Clear for a fresh pass: no reserved bytes, feature off. Keeps the backing (only the bits move).
void zeropage_reset(zeropage *zp);

// Switch the feature on without reserving any byte (ZA_POOL with an empty list still enables it).
void zeropage_enable(zeropage *zp);

// Reserve one zero-page byte for auto-allocation (idempotent) and enable the feature. byte must be
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

// The index of the variable declared in scope at def, or RC_INDEX_NONE if that pair is not a ZA_AUTO
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
