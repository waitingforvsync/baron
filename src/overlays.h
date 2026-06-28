#ifndef BARON_OVERLAYS_H_
#define BARON_OVERLAYS_H_

#include "richc/bytes.h"


// One overlay: a contiguous block of object code with its own instruction pointer. pc is the
// effective address of the next byte - the value a label takes. The byte array fills from index 0
// independent of pc, so ORG repositions later labels without moving where code lands.
typedef struct overlay {
    uint32_t       pc;
    rc_array_bytes code;
} overlay;

#define RC_ARRAY_TYPE overlay
#define RC_ARRAY_NAME overlay
#include "richc/template/array.h"


// The overlay manager: a container of overlays addressed by index, in the same spirit as `scopes`
// is for symbols. Index 0 is the default overlay; for now only the default is used (more arrive
// with the overlay directives). Every per-overlay operation goes through the manager and takes an
// index. The code_arena is shared by all overlays' object code, which is fine while only the
// default is emitted into; genuine multi-overlay assembly will want a code arena per overlay.
typedef struct overlays {
    rc_arena         node_arena;   // backs the nodes array
    rc_arena         code_arena;   // backs the overlays' object code
    rc_array_overlay nodes;        // index 0 is the default overlay
} overlays;

enum { overlays_default = 0 };   // index of the default overlay

void     overlays_init(overlays *ovl);
void     overlays_deinit(overlays *ovl);
uint32_t overlays_make_default(overlays *ovl);    // create overlay 0; returns its index (0)

// Per-overlay queries.
uint32_t      overlays_pc(const overlays *ovl, uint32_t id);
rc_view_bytes overlays_code(const overlays *ovl, uint32_t id);

// Per-overlay mutation (manager + index, as per scopes).
void overlays_org(overlays *ovl, uint32_t id, uint32_t addr);     // set pc; does not move code
void overlays_emit_u8(overlays *ovl, uint32_t id, uint8_t b);     // append a byte, pc += 1
void overlays_emit_u16(overlays *ovl, uint32_t id, uint16_t w);   // little-endian word, pc += 2
void overlays_skip(overlays *ovl, uint32_t id, uint32_t count);   // append `count` zero bytes, pc += count

// Reset every overlay (pc = 0, code emptied, buffers kept) for a fresh pass.
void overlays_reset_all(overlays *ovl);


#endif // ifndef BARON_OVERLAYS_H_
