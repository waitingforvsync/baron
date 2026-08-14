#include "sections.h"

#include "richc/macros.h"
#include "richc/mstr.h"   // rc_mstr_from_str: durable copies of a section's name / attribute keys


enum {
    sections_nodes_reserve = 64,        // 64 ROM banks / sections is a very large project
    section_code_reserve   = 0x10000,   // 64 KB: the hard 6502 ceiling, so a code buffer never grows within a pass
    section_attrs_reserve  = 4,         // a handful of attributes per section is typical
};

void sections_init(sections *sec, rc_arena *per_pass)
{
    RC_ASSERT(sec != NULL && per_pass != NULL);
    sec->arena = per_pass;          // borrowed; baron owns it
    sec->nodes = (rc_array_section) {0};   // sections_reset builds the list each pass
}

void sections_reset(sections *sec)
{
    RC_ASSERT(sec != NULL);
    sec->nodes = rc_array_section_make(sections_nodes_reserve, sec->arena);
    rc_array_section_push(                                   // the default section is index 0
        &sec->nodes,
        (section) { .code = rc_array_bytes_make(section_code_reserve, sec->arena) },
        sec->arena);
}

uint32_t sections_pc(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return RC_AT(sec->nodes, id).pc;
}

rc_view_bytes sections_code(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return RC_AT(sec->nodes, id).code.view;
}

rc_view_section sections_all(const sections *sec)
{
    RC_ASSERT(sec != NULL);
    return sec->nodes.view;
}

rc_view_attribute sections_attributes(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return RC_AT(sec->nodes, id).attributes.view;
}

uint32_t sections_make(sections *sec, rc_str name)
{
    RC_ASSERT(sec != NULL);
    // Sections are few (one per named region), so a linear scan by name is ample. A repeated name is a
    // duplicate: hand back RC_INDEX_NONE and let the caller diagnose it. Skip nameless entries - the default
    // at index 0 has a zero-initialised {0} name (NULL data, which rc_str_is_equal rejects) and is never a
    // target of a name lookup anyway.
    for (uint32_t i = 0; i < sec->nodes.num; i++) {
        rc_str n = RC_AT(sec->nodes, i).name;
        if (n.len != 0 && rc_str_is_equal(n, name)) {
            return RC_INDEX_NONE;   // already defined this pass
        }
    }
    return rc_array_section_push(
        &sec->nodes,
        (section) { .name = name, .code = rc_array_bytes_make(section_code_reserve, sec->arena) },
        sec->arena);
}

void sections_add_attribute(sections *sec, uint32_t id, rc_str key, value v, cursor at)
{
    RC_ASSERT(sec != NULL);
    section *s = &RC_AT(sec->nodes, id);
    value copy = value_make_copy(v, sec->arena);   // durable backing in the (per-pass) manager arena

    // Upsert: a key already present (inherited from a parent, most often) is replaced in place; otherwise
    // append. The bag is made lazily so a section with no attributes carries none.
    for (uint32_t i = 0; i < s->attributes.num; i++) {
        if (rc_str_is_equal(RC_AT(s->attributes, i).key, key)) {
            RC_AT(s->attributes, i) = (attribute) { .key = key, .v = copy, .at = at };
            return;
        }
    }
    if (s->attributes.data == NULL) {
        s->attributes = rc_array_attribute_make(section_attrs_reserve, sec->arena);
    }
    rc_array_attribute_push(&s->attributes, (attribute) { .key = key, .v = copy, .at = at }, sec->arena);
}

void sections_org(sections *sec, uint32_t id, uint32_t addr)
{
    RC_ASSERT(sec != NULL);
    RC_AT(sec->nodes, id).pc = addr;   // only the effective address moves; code still appends at code.num
}

void sections_emit_u8(sections *sec, uint32_t id, uint8_t b)
{
    RC_ASSERT(sec != NULL);
    section *s = &RC_AT(sec->nodes, id);
    rc_array_bytes_push(&s->code, b, sec->arena);
    s->pc += 1;
}

void sections_emit_u16(sections *sec, uint32_t id, uint16_t w)
{
    sections_emit_u8(sec, id, (uint8_t)(w & 0xFF));   // little-endian: low byte first
    sections_emit_u8(sec, id, (uint8_t)(w >> 8));
}

void sections_skip(sections *sec, uint32_t id, uint32_t count)
{
    RC_ASSERT(sec != NULL);
    section *s = &RC_AT(sec->nodes, id);
    for (uint32_t i = 0; i < count; i++) {
        rc_array_bytes_push(&s->code, 0, sec->arena);
    }
    s->pc += count;
}

void sections_patch_add_u8(sections *sec, uint32_t id, uint32_t offset, uint8_t delta)
{
    RC_ASSERT(sec != NULL);
    section *s = &RC_AT(sec->nodes, id);
    uint8_t *byte = &RC_AT(s->code, offset);
    *byte = (uint8_t) (*byte + delta);
}

// Copy an rc_str into the arena. The nameless default's {0} name stays {0} - there is nothing to own.
static rc_str str_make_copy(rc_str s, rc_arena *arena)
{
    if (s.len == 0) {
        return (rc_str) {0};
    }
    return rc_mstr_from_str(s, 0, arena).view;
}

section section_make_copy(section s, rc_arena *arena)
{
    RC_ASSERT(arena != NULL);
    // Everything the section references is copied, not just the code: the name and attribute keys are views
    // into source text held by a DIFFERENT arena, and a copy that borrowed them would quietly die with it.
    section copy = {
        .name = str_make_copy(s.name, arena),
        .pc   = s.pc,
        .code = rc_array_bytes_make_copy(s.code.view, 0, arena),
    };
    if (s.attributes.num != 0) {
        copy.attributes = rc_array_attribute_make(s.attributes.num, arena);
        for (uint32_t i = 0; i < s.attributes.num; i++) {
            attribute a = RC_AT(s.attributes, i);
            rc_array_attribute_push(
                &copy.attributes,
                (attribute) {.key = str_make_copy(a.key, arena), .v = value_make_copy(a.v, arena), .at = a.at},
                arena);
        }
    }
    return copy;
}

#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(sections, emit_org_reset)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena);
    sections_reset(&sec);   // builds the default section at index 0

    sections_emit_u8(&sec, 0, 0xA9);
    RC_CHECK(sections_pc(&sec, 0), ==, 1u);
    RC_CHECK(sections_code(&sec, 0).num, ==, 1u);

    sections_emit_u16(&sec, 0, 0x1234);          // little-endian
    RC_CHECK(sections_pc(&sec, 0), ==, 3u);
    RC_CHECK((uint32_t)rc_view_bytes_get(sections_code(&sec, 0), 1), ==, 0x34u);
    RC_CHECK((uint32_t)rc_view_bytes_get(sections_code(&sec, 0), 2), ==, 0x12u);

    // A new address (org) moves pc only; the next byte still lands at code index 3.
    sections_org(&sec, 0, 0x2000);
    RC_CHECK(sections_pc(&sec, 0), ==, 0x2000u);
    RC_CHECK(sections_code(&sec, 0).num, ==, 3u);
    sections_emit_u8(&sec, 0, 0xEA);
    RC_CHECK(sections_pc(&sec, 0), ==, 0x2001u);
    RC_CHECK(sections_code(&sec, 0).num, ==, 4u);

    // SKIP appends that many zero bytes and advances pc by the same.
    sections_skip(&sec, 0, 3);
    RC_CHECK(sections_pc(&sec, 0), ==, 0x2004u);
    RC_CHECK(sections_code(&sec, 0).num, ==, 7u);
    RC_CHECK((uint32_t)rc_view_bytes_get(sections_code(&sec, 0), 4), ==, 0x00u);

    // Reset rebuilds a fresh default section: pc 0, empty code.
    sections_reset(&sec);
    RC_CHECK(sections_pc(&sec, 0), ==, 0u);
    RC_CHECK(sections_code(&sec, 0).num, ==, 0u);

    rc_arena_deinit(&arena);
}

RC_TEST(sections, make_unique_and_attributes)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena);
    sections_reset(&sec);

    uint32_t a = sections_make(&sec, RC_STR("code"));
    RC_CHECK(a, ==, 1u);                                  // first named section after the default
    uint32_t d = sections_make(&sec, RC_STR("data"));
    RC_CHECK(d, ==, 2u);
    RC_CHECK_TRUE(sections_make(&sec, RC_STR("code")) == RC_INDEX_NONE);   // a repeat is refused

    sections_add_attribute(&sec, a, RC_STR("load"), value_make_numeric(0x1200), cursor_none());
    RC_CHECK(sections_attributes(&sec, a).num, ==, 1u);
    sections_add_attribute(&sec, a, RC_STR("load"), value_make_numeric(0x1900), cursor_none());   // upsert
    RC_CHECK(sections_attributes(&sec, a).num, ==, 1u);
    RC_CHECK(rc_view_attribute_get(sections_attributes(&sec, a), 0).v.numeric, ==, 6400.0);   // 0x1900

    rc_arena_deinit(&arena);
}

RC_TEST(sections, make_copy_owns_its_backing)
{
    rc_arena original = rc_arena_make_default();
    rc_arena kept     = rc_arena_make_default();

    // Build the strings in the original arena rather than using literals, so a copy that merely borrowed
    // them would genuinely dangle once that arena dies (a literal never dies, and would mask the bug).
    rc_str name = rc_mstr_from_cstr("code", 0, &original).view;
    rc_str key  = rc_mstr_from_cstr("filename", 0, &original).view;
    rc_str file = rc_mstr_from_cstr("game.bin", 0, &original).view;

    sections sec;
    sections_init(&sec, &original);
    sections_reset(&sec);
    uint32_t id = sections_make(&sec, name);
    sections_org(&sec, id, 0x1900);
    sections_emit_u8(&sec, id, 0xA9);
    sections_emit_u8(&sec, id, 0x2A);
    sections_add_attribute(&sec, id, key, value_make_string(file), cursor_none());

    section copy = section_make_copy(RC_AT(sec.nodes, id), &kept);

    // The decisive move: kill the arena everything was built in. If the copy borrowed any of it, the reads
    // below touch freed memory (which the sanitizer build turns into a hard failure).
    rc_arena_deinit(&original);

    RC_CHECK(copy.name, ==, RC_STR("code"));
    RC_CHECK(copy.pc, ==, 0x1902u);
    RC_CHECK(copy.code.num, ==, 2u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 0), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 1), ==, 0x2Au);
    RC_CHECK(copy.attributes.num, ==, 1u);
    RC_CHECK(RC_AT(copy.attributes, 0).key, ==, RC_STR("filename"));
    RC_CHECK_TRUE(value_is_equal(RC_AT(copy.attributes, 0).v, value_make_string(RC_STR("game.bin"))));

    rc_arena_deinit(&kept);
}

#endif // BARON_TESTS
