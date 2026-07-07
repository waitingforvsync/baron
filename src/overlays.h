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

// Per-overlay mutation (manager + index, as per scopes).
void overlays_org(overlays *ovl, uint32_t id, uint32_t addr);     // set pc; does not move code
void overlays_emit_u8(overlays *ovl, uint32_t id, uint8_t b);     // append a byte, pc += 1
void overlays_emit_u16(overlays *ovl, uint32_t id, uint16_t w);   // little-endian word, pc += 2
void overlays_skip(overlays *ovl, uint32_t id, uint32_t count);   // append `count` zero bytes, pc += count

// Rebuild the list for a fresh pass: the default overlay at index 0 (pc 0, an empty code buffer). Call
// it at the top of each pass, after the per_pass arena has been reset. It also makes the default the
// first time round (there is no separate overlays_make_default).
void overlays_reset(overlays *ovl);


#endif // ifndef BARON_OVERLAYS_H_
