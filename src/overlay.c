#include "overlay.h"

#include "richc/macros.h"


void overlay_init(overlay *o, rc_arena *arena)
{
    RC_ASSERT(o != NULL && arena != NULL);
    o->pc    = 0;
    o->code  = rc_array_bytes_make(0, arena);
    o->arena = arena;
}

void overlay_reset(overlay *o)
{
    RC_ASSERT(o != NULL);
    o->pc = 0;
    rc_array_bytes_reset(&o->code);   // keep the buffer; each pass refills it from the start
}

void overlay_org(overlay *o, uint32_t addr)
{
    RC_ASSERT(o != NULL);
    o->pc = addr;      // only the effective address moves; code still appends at code.num
}

void overlay_emit_u8(overlay *o, uint8_t b)
{
    RC_ASSERT(o != NULL);
    rc_array_bytes_push(&o->code, b, o->arena);
    o->pc += 1;
}

void overlay_emit_u16(overlay *o, uint16_t w)
{
    overlay_emit_u8(o, (uint8_t)(w & 0xFF));   // little-endian: low byte first
    overlay_emit_u8(o, (uint8_t)(w >> 8));
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(overlay, emit_and_org)
{
    rc_arena arena = rc_arena_make_default();
    overlay  o;
    overlay_init(&o, &arena);

    overlay_emit_u8(&o, 0xA9);
    RC_CHECK(o.pc, ==, 1u);
    RC_CHECK(o.code.num, ==, 1u);

    overlay_emit_u16(&o, 0x1234);          // little-endian
    RC_CHECK(o.pc, ==, 3u);
    RC_CHECK(o.code.num, ==, 3u);
    RC_CHECK((uint32_t)rc_view_bytes_get(o.code.view, 1), ==, 0x34u);
    RC_CHECK((uint32_t)rc_view_bytes_get(o.code.view, 2), ==, 0x12u);

    // ORG moves pc only; the next byte still lands at code index 3.
    overlay_org(&o, 0x2000);
    RC_CHECK(o.pc, ==, 0x2000u);
    RC_CHECK(o.code.num, ==, 3u);
    overlay_emit_u8(&o, 0xEA);
    RC_CHECK(o.pc, ==, 0x2001u);
    RC_CHECK(o.code.num, ==, 4u);

    // Reset empties the code and pc but keeps the buffer for reuse.
    overlay_reset(&o);
    RC_CHECK(o.pc, ==, 0u);
    RC_CHECK(o.code.num, ==, 0u);

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
