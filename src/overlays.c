#include "overlays.h"

#include "richc/macros.h"


void overlays_init(overlays *ovl)
{
    RC_ASSERT(ovl != NULL);
    ovl->node_arena = rc_arena_make_default();
    ovl->code_arena = rc_arena_make_default();
    ovl->nodes      = rc_array_overlay_make(0, &ovl->node_arena);
}

void overlays_deinit(overlays *ovl)
{
    RC_ASSERT(ovl != NULL);
    rc_arena_deinit(&ovl->node_arena);
    rc_arena_deinit(&ovl->code_arena);
}

uint32_t overlays_make_default(overlays *ovl)
{
    RC_ASSERT(ovl != NULL && rc_array_overlay_is_empty(&ovl->nodes));   // the default is the first overlay
    return rc_array_overlay_push(
        &ovl->nodes,
        (overlay) { .code = rc_array_bytes_make(0, &ovl->code_arena) },
        &ovl->node_arena);
}

uint32_t overlays_pc(const overlays *ovl, uint32_t id)
{
    RC_ASSERT(ovl != NULL);
    return RC_AT(ovl->nodes, id).pc;
}

rc_view_bytes overlays_code(const overlays *ovl, uint32_t id)
{
    RC_ASSERT(ovl != NULL);
    return RC_AT(ovl->nodes, id).code.view;
}

void overlays_org(overlays *ovl, uint32_t id, uint32_t addr)
{
    RC_ASSERT(ovl != NULL);
    RC_AT(ovl->nodes, id).pc = addr;   // only the effective address moves; code still appends at code.num
}

void overlays_emit_u8(overlays *ovl, uint32_t id, uint8_t b)
{
    RC_ASSERT(ovl != NULL);
    overlay *o = &RC_AT(ovl->nodes, id);
    rc_array_bytes_push(&o->code, b, &ovl->code_arena);
    o->pc += 1;
}

void overlays_emit_u16(overlays *ovl, uint32_t id, uint16_t w)
{
    overlays_emit_u8(ovl, id, (uint8_t)(w & 0xFF));   // little-endian: low byte first
    overlays_emit_u8(ovl, id, (uint8_t)(w >> 8));
}

void overlays_reset_all(overlays *ovl)
{
    RC_ASSERT(ovl != NULL);
    for (uint32_t i = 0; i < ovl->nodes.num; i++) {
        overlay *o = &RC_AT(ovl->nodes, i);
        o->pc = 0;
        rc_array_bytes_reset(&o->code);   // keep the buffer; refilled from the start each pass
    }
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(overlays, emit_org_reset)
{
    overlays ovl;
    overlays_init(&ovl);
    RC_CHECK(overlays_make_default(&ovl), ==, 0u);

    overlays_emit_u8(&ovl, 0, 0xA9);
    RC_CHECK(overlays_pc(&ovl, 0), ==, 1u);
    RC_CHECK(overlays_code(&ovl, 0).num, ==, 1u);

    overlays_emit_u16(&ovl, 0, 0x1234);          // little-endian
    RC_CHECK(overlays_pc(&ovl, 0), ==, 3u);
    RC_CHECK((uint32_t)rc_view_bytes_get(overlays_code(&ovl, 0), 1), ==, 0x34u);
    RC_CHECK((uint32_t)rc_view_bytes_get(overlays_code(&ovl, 0), 2), ==, 0x12u);

    // ORG moves pc only; the next byte still lands at code index 3.
    overlays_org(&ovl, 0, 0x2000);
    RC_CHECK(overlays_pc(&ovl, 0), ==, 0x2000u);
    RC_CHECK(overlays_code(&ovl, 0).num, ==, 3u);
    overlays_emit_u8(&ovl, 0, 0xEA);
    RC_CHECK(overlays_pc(&ovl, 0), ==, 0x2001u);
    RC_CHECK(overlays_code(&ovl, 0).num, ==, 4u);

    // Reset empties every overlay's code and pc but keeps the buffer.
    overlays_reset_all(&ovl);
    RC_CHECK(overlays_pc(&ovl, 0), ==, 0u);
    RC_CHECK(overlays_code(&ovl, 0).num, ==, 0u);

    overlays_deinit(&ovl);
}

#endif // BARON_TESTS
