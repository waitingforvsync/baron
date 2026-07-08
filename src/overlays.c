#include "overlays.h"

#include "richc/macros.h"


enum {
    overlays_nodes_reserve = 64,        // 64 ROM banks / sections is a very large project
    overlay_code_reserve   = 0x10000,   // 64 KB: the hard 6502 ceiling, so a code buffer never grows within a pass
};

void overlays_init(overlays *ovl, rc_arena *per_pass)
{
    RC_ASSERT(ovl != NULL && per_pass != NULL);
    ovl->arena = per_pass;          // borrowed; baron owns it
    ovl->nodes = (rc_array_overlay) {0};   // overlays_reset builds the list each pass
}

void overlays_reset(overlays *ovl)
{
    RC_ASSERT(ovl != NULL);
    ovl->nodes = rc_array_overlay_make(overlays_nodes_reserve, ovl->arena);
    rc_array_overlay_push(                                   // the default overlay is index 0
        &ovl->nodes,
        (overlay) { .code = rc_array_bytes_make(overlay_code_reserve, ovl->arena) },
        ovl->arena);
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

rc_view_overlay overlays_all(const overlays *ovl)
{
    RC_ASSERT(ovl != NULL);
    return ovl->nodes.view;
}

uint32_t overlays_get_or_make(overlays *ovl, rc_str name)
{
    RC_ASSERT(ovl != NULL);
    // Overlays are few (one per section), so a linear scan by name is ample. Skip nameless entries - the
    // default at index 0 has a zero-initialised {0} name (NULL data, which rc_str_is_equal rejects), and it
    // is never reachable by a name lookup regardless.
    for (uint32_t i = 0; i < ovl->nodes.num; i++) {
        rc_str n = RC_AT(ovl->nodes, i).name;
        if (n.len != 0 && rc_str_is_equal(n, name)) {
            return i;   // seen already this pass: reuse it, so its code + pc keep accumulating
        }
    }
    return rc_array_overlay_push(
        &ovl->nodes,
        (overlay) { .name = name, .code = rc_array_bytes_make(overlay_code_reserve, ovl->arena) },
        ovl->arena);
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
    rc_array_bytes_push(&o->code, b, ovl->arena);
    o->pc += 1;
}

void overlays_emit_u16(overlays *ovl, uint32_t id, uint16_t w)
{
    overlays_emit_u8(ovl, id, (uint8_t)(w & 0xFF));   // little-endian: low byte first
    overlays_emit_u8(ovl, id, (uint8_t)(w >> 8));
}

void overlays_skip(overlays *ovl, uint32_t id, uint32_t count)
{
    RC_ASSERT(ovl != NULL);
    overlay *o = &RC_AT(ovl->nodes, id);
    for (uint32_t i = 0; i < count; i++) {
        rc_array_bytes_push(&o->code, 0, ovl->arena);
    }
    o->pc += count;
}

void overlays_patch_add_u8(overlays *ovl, uint32_t id, uint32_t offset, uint8_t delta)
{
    RC_ASSERT(ovl != NULL);
    overlay *o = &RC_AT(ovl->nodes, id);
    uint8_t *byte = &RC_AT(o->code, offset);
    *byte = (uint8_t) (*byte + delta);
}

#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(overlays, emit_org_reset)
{
    rc_arena arena = rc_arena_make_default();
    overlays ovl;
    overlays_init(&ovl, &arena);
    overlays_reset(&ovl);   // builds the default overlay at index 0

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

    // SKIP appends that many zero bytes and advances pc by the same.
    overlays_skip(&ovl, 0, 3);
    RC_CHECK(overlays_pc(&ovl, 0), ==, 0x2004u);
    RC_CHECK(overlays_code(&ovl, 0).num, ==, 7u);
    RC_CHECK((uint32_t)rc_view_bytes_get(overlays_code(&ovl, 0), 4), ==, 0x00u);

    // Reset rebuilds a fresh default overlay: pc 0, empty code.
    overlays_reset(&ovl);
    RC_CHECK(overlays_pc(&ovl, 0), ==, 0u);
    RC_CHECK(overlays_code(&ovl, 0).num, ==, 0u);

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
