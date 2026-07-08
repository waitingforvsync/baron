#ifndef BARON_OVERLAYS_H_
#define BARON_OVERLAYS_H_

#include "richc/bytes.h"


// One overlay: a contiguous block of object code with its own instruction pointer. pc is the
// effective address of the next byte - the value a label takes. The byte array fills from index 0
// independent of pc, so ORG repositions later labels without moving where code lands. `name` is the
// overlay's own identity in a namespace SEPARATE from symbols and scopes (the default overlay at index 0
// is nameless, {0}); OVERLAY <name> selects it. The name is a view into the source text (permanent), so
// it outlives the per-pass overlay it labels.
typedef struct overlay {
    rc_str         name;
    uint32_t       pc;
    rc_array_bytes code;
} overlay;

#define RC_ARRAY_TYPE overlay
#define RC_ARRAY_NAME overlay
#include "richc/template/array.h"


// The overlay manager: a container of overlays addressed by index, in the same spirit as `scopes`
// is for symbols. Index 0 is the default overlay. Every per-overlay operation goes through the manager
// and takes an index. Overlays are PER-PASS: nothing about them needs to survive a pass (object code
// is only read from the final pass's output, and pc resets every pass), so the whole manager - list
// and per-overlay code alike - lives in the BORROWED per_pass arena and is rebuilt by overlays_reset
// at the top of each pass (after that arena is reset once).
typedef struct overlays {
    rc_arena        *arena;   // BORROWED: baron's per_pass arena, backs the nodes AND every overlay's code
    rc_array_overlay nodes;   // index 0 is the default overlay
} overlays;

enum { overlays_default = 0 };   // index of the default overlay

void overlays_init(overlays *ovl, rc_arena *per_pass);

// Per-overlay queries.
uint32_t      overlays_pc(const overlays *ovl, uint32_t id);
rc_view_bytes overlays_code(const overlays *ovl, uint32_t id);

// The whole overlay list as a read-only view (index 0 is the default). This is what a baron_result hands
// back so a caller can see every overlay's pc + code, not just the default one's bytes.
rc_view_overlay overlays_all(const overlays *ovl);

// Per-overlay mutation (manager + index, as per scopes).
void overlays_org(overlays *ovl, uint32_t id, uint32_t addr);     // set pc; does not move code
void overlays_emit_u8(overlays *ovl, uint32_t id, uint8_t b);     // append a byte, pc += 1
void overlays_emit_u16(overlays *ovl, uint32_t id, uint16_t w);   // little-endian word, pc += 2
void overlays_skip(overlays *ovl, uint32_t id, uint32_t count);   // append `count` zero bytes, pc += count

// Post-hoc patch: add `delta` (mod 256) to the byte already emitted at `offset` in overlay `id`. Used by the
// zero-page allocator to fold a variable's assigned base address into an operand that was emitted with the
// placeholder base (so the emitted byte held just the intra-variable offset). Does NOT touch pc.
void overlays_patch_add_u8(overlays *ovl, uint32_t id, uint32_t offset, uint8_t delta);

// Select the overlay named `name`, making it (with its own pc 0 + empty code buffer) on first sighting;
// hands back its stable index. Overlay names live in their OWN namespace, keyed by content. `name` must be
// non-empty (the nameless default is index 0, unreachable this way). The index is stable across passes
// because overlays are created in first-sighting parse order, which is identical each pass.
uint32_t overlays_get_or_make(overlays *ovl, rc_str name);

// Rebuild the list for a fresh pass: the default overlay at index 0 (nameless, pc 0, an empty code buffer).
// Call it at the top of each pass, after the per_pass arena has been reset. It also makes the default the
// first time round (there is no separate overlays_make_default).
void overlays_reset(overlays *ovl);


#endif // ifndef BARON_OVERLAYS_H_
