#ifndef BARON_OVERLAY_H_
#define BARON_OVERLAY_H_

#include "richc/bytes.h"   // rc_array_bytes / rc_view_bytes


// An overlay is one contiguous block of object code with its own instruction pointer. The
// pc is the effective address at which the next byte is taken to live - the value a label
// assumes. The byte array is independent of that address: it always fills from index 0, so
// moving pc with ORG repositions later labels without moving where code lands. (For now
// there is a single default overlay; more, with attributes, arrive in a later milestone.)
typedef struct overlay {
    uint32_t       pc;     // instruction pointer / effective address of the next byte
    rc_array_bytes code;   // object code, contiguous from index 0
    rc_arena      *arena;  // backs `code`
} overlay;

void overlay_init(overlay *o, rc_arena *arena);
void overlay_reset(overlay *o);                 // pc = 0, code emptied (capacity kept); per pass
void overlay_org(overlay *o, uint32_t addr);    // set pc only; does not move where code lands
void overlay_emit_u8(overlay *o, uint8_t b);    // append one byte, pc += 1
void overlay_emit_u16(overlay *o, uint16_t w);  // append a little-endian word, pc += 2


#endif // ifndef BARON_OVERLAY_H_
