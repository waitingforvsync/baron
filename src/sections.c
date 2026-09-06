#include "sections.h"

#include "richc/macros.h"
#include "richc/mstr.h"


enum {
    sections_nodes_reserve  = 64,        // 64 ROM banks / sections is a very large project
    sections_stream_reserve = 0x10000,   // 64 KB: the hard 6502 ceiling, so the stream rarely grows within a pass
    section_attrs_reserve   = 4,         // a handful of attributes per section is typical
};


// ---- lifecycle ----

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent)
{
    RC_ASSERT(sec != NULL && per_pass != NULL && permanent != NULL);

    // The stream and node list stay empty here: sections_reset builds them at the top of each pass.
    // The emission fingerprints are lazily made and deliberately survive the per-pass resets.
    sec->arena     = per_pass;
    sec->permanent = permanent;
    sec->stream    = (rc_array_bytes) {0};
    sec->nodes     = (rc_array_section) {0};
    sec->emissions = (rc_view_section_emission) {0};
}


void sections_reset(sections *sec)
{
    RC_ASSERT(sec != NULL);

    // A fresh stream, and the default section at index 0: an empty window at its start
    sec->stream = rc_array_bytes_make(sections_stream_reserve, sec->arena);
    sec->nodes  = rc_array_section_make(sections_nodes_reserve, sec->arena);
    rc_array_section_push(&sec->nodes, (section) {0}, sec->arena);
}


// ---- queries ----

uint32_t sections_pc(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return rc_array_section_get(&sec->nodes, id).pc;
}


rc_view_bytes sections_code(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);

    section s = rc_array_section_get(&sec->nodes, id);
    return rc_view_bytes_get_subview(sec->stream.view, s.begin, s.end);
}


rc_view_section sections_all(const sections *sec)
{
    RC_ASSERT(sec != NULL);
    return sec->nodes.view;
}


rc_view_attribute sections_attributes(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return rc_array_section_get(&sec->nodes, id).attributes.view;
}


uint32_t sections_find(const sections *sec, rc_str name)
{
    RC_ASSERT(sec != NULL);

    // Sections are few, so a linear scan is ample. Skip nameless entries: the default's {0} name has
    // NULL data (which rc_str_is_equal rejects) and is never a lookup target anyway.
    for (uint32_t i = 0; i < sec->nodes.num; i++) {
        rc_str n = rc_array_section_get(&sec->nodes, i).name;
        if (n.len != 0 && rc_str_is_equal(n, name)) {
            return i;
        }
    }

    return RC_INDEX_NONE;
}


bool sections_cmos(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return rc_array_section_get(&sec->nodes, id).cmos;
}


bool sections_is_guarded(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return rc_array_section_get(&sec->nodes, id).is_guarded;
}


uint32_t sections_guard(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL && sections_is_guarded(sec, id));
    return rc_array_section_get(&sec->nodes, id).guard;
}


// ---- mutation ----

uint32_t sections_make(sections *sec, rc_str name)
{
    RC_ASSERT(sec != NULL);

    // A repeated name is a duplicate: hand back RC_INDEX_NONE and let the caller diagnose it
    if (sections_find(sec, name) != RC_INDEX_NONE) {
        return RC_INDEX_NONE;
    }

    return rc_array_section_push(
        &sec->nodes,
        (section) { .name = name, .begin = sec->stream.num, .end = sec->stream.num },
        sec->arena);
}


void sections_add_attribute(sections *sec, uint32_t id, rc_str key, value v, cursor at)
{
    RC_ASSERT(sec != NULL);

    section *s = rc_array_section_at(&sec->nodes, id);
    value copy = value_make_copy(v, sec->arena);   // durable backing in the (per-pass) manager arena

    // Upsert: a key already present (inherited from a parent, most often) is replaced in place
    for (uint32_t i = 0; i < s->attributes.num; i++) {
        if (rc_str_is_equal(rc_array_attribute_get(&s->attributes, i).key, key)) {
            *rc_array_attribute_at(&s->attributes, i) = (attribute) {
                .key = key,
                .v   = copy,
                .at  = at,
            };
            return;
        }
    }

    // Otherwise append; the bag is made lazily so a section with no attributes carries none
    if (s->attributes.data == NULL) {
        s->attributes = rc_array_attribute_make(section_attrs_reserve, sec->arena);
    }

    rc_array_attribute_push(&s->attributes, (attribute) {
                .key = key,
                .v   = copy,
                .at  = at,
            }, sec->arena);
}


void sections_org(sections *sec, uint32_t id, uint32_t addr)
{
    RC_ASSERT(sec != NULL);

    // Only the effective address moves, and only the 6502's 16 bits of it (the high half of a BBC
    // host address is which-processor flavouring, not somewhere a pc can point)
    rc_array_section_at(&sec->nodes, id)->pc = addr & 0xFFFF;
}


void sections_set_cmos(sections *sec, uint32_t id, bool cmos)
{
    RC_ASSERT(sec != NULL);
    rc_array_section_at(&sec->nodes, id)->cmos = cmos;
}


void sections_set_guard(sections *sec, uint32_t id, uint32_t addr)
{
    RC_ASSERT(sec != NULL);

    section *s = rc_array_section_at(&sec->nodes, id);
    s->is_guarded = true;
    s->guard      = addr & 0xFFFF;   // the pc it is compared against is 16-bit too
}


void sections_emit_u8(sections *sec, uint32_t id, uint8_t b)
{
    RC_ASSERT(sec != NULL);

    // Emission only ever happens in the innermost open section, so its window tail IS the stream tail
    rc_array_bytes_push(&sec->stream, b, sec->arena);
    section *s = rc_array_section_at(&sec->nodes, id);
    s->end = sec->stream.num;
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

    rc_array_bytes_push_n_zero(&sec->stream, count, sec->arena);
    section *s = rc_array_section_at(&sec->nodes, id);
    s->end = sec->stream.num;
    s->pc += count;
}


void sections_close(sections *sec, uint32_t id, uint32_t parent)
{
    RC_ASSERT(sec != NULL && id != parent);

    // The child's window is contiguous and sits at the parent's tail, so absorbing it is just
    // bookkeeping: extend the window, advance the pc by the child's size
    section c  = rc_array_section_get(&sec->nodes, id);
    section *p = rc_array_section_at(&sec->nodes, parent);
    p->end = c.end;
    p->pc += c.end - c.begin;
}


void sections_seal(sections *sec)
{
    RC_ASSERT(sec != NULL);

    for (uint32_t i = 0; i < sec->nodes.num; i++) {
        section *s = rc_array_section_at(&sec->nodes, i);
        s->code = rc_view_bytes_get_subview(sec->stream.view, s->begin, s->end);
    }
}


// ---- cross-pass bookkeeping ----

// CRC-32 (the standard reflected polynomial), bitwise - no table. The inputs are one program's object
// code once per settling pass, so simplicity beats speed.
static uint32_t crc32_bytes(rc_view_bytes bytes)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < bytes.num; i++) {
        crc ^= rc_view_bytes_get(bytes, i);
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }

    return ~crc;
}


bool sections_emission_changed(sections *sec)
{
    RC_ASSERT(sec != NULL);

    bool first   = sec->emissions.num == 0;   // nothing noted yet: the first pass has no comparison
    bool changed = !first && sec->emissions.num != sec->nodes.num;

    // Compare each section's fingerprint against last time's while rebuilding the noted set for next
    // time. Windows overlap (a parent's covers its children's), so a child's shift also fingerprints
    // through its ancestors - redundant, but only ever MORE eager, and index order is pass-stable.
    rc_array_section_emission next = rc_array_section_emission_make(sec->nodes.num, sec->permanent);
    for (uint32_t i = 0; i < sec->nodes.num; i++) {
        section_emission e = {
            .size = sections_code(sec, i).num,
            .crc  = crc32_bytes(sections_code(sec, i)),
        };
        if (!first && i < sec->emissions.num) {
            section_emission prev = rc_view_section_emission_get(sec->emissions, i);
            changed = changed || prev.size != e.size || prev.crc != e.crc;
        }
        rc_array_section_emission_push(&next, e, sec->permanent);
    }

    sec->emissions = next.view;
    return changed;
}


// ---- copying ----

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
    // into source text held by a DIFFERENT arena, and a copy that borrowed them would quietly die with it
    rc_array_attribute attributes = {0};
    if (s.attributes.num != 0) {
        attributes = rc_array_attribute_make(s.attributes.num, arena);
        for (uint32_t i = 0; i < s.attributes.num; i++) {
            attribute a = rc_array_attribute_get(&s.attributes, i);
            rc_array_attribute_push(
                &attributes,
                (attribute) {
                    .key = str_make_copy(a.key, arena),
                    .v   = value_make_copy(a.v, arena),
                    .at  = a.at,
                },
                arena);
        }
    }

    // The copied window is normalized to its own bytes: begin/end are meaningless without the stream
    return (section) {
        .name       = str_make_copy(s.name, arena),
        .pc         = s.pc,
        .cmos       = s.cmos,
        .is_guarded = s.is_guarded,
        .guard      = s.guard,
        .begin      = 0,
        .end        = s.code.num,
        .code       = rc_array_bytes_make_copy(s.code, 0, arena).view,
        .attributes = attributes,
    };
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(sections, emission_changed_tracks_content)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);

    // Pass 1: nothing to compare against, whatever we emitted.
    sections_reset(&sec);
    sections_emit_u8(&sec, 0, 0xA9);
    sections_emit_u8(&sec, 0, 0x42);
    RC_CHECK_FALSE(sections_emission_changed(&sec));

    // Pass 2, identical emission: no change.
    sections_reset(&sec);
    sections_emit_u8(&sec, 0, 0xA9);
    sections_emit_u8(&sec, 0, 0x42);
    RC_CHECK_FALSE(sections_emission_changed(&sec));

    // Pass 3, same LENGTH but different content: the crc notices.
    sections_reset(&sec);
    sections_emit_u8(&sec, 0, 0xA9);
    sections_emit_u8(&sec, 0, 0x43);
    RC_CHECK_TRUE(sections_emission_changed(&sec));

    // Pass 4, same content but a new section appears: the count notices.
    sections_reset(&sec);
    sections_emit_u8(&sec, 0, 0xA9);
    sections_emit_u8(&sec, 0, 0x43);
    sections_make(&sec, RC_STR("extra"));
    RC_CHECK_TRUE(sections_emission_changed(&sec));

    // Pass 5, stable again.
    sections_reset(&sec);
    sections_emit_u8(&sec, 0, 0xA9);
    sections_emit_u8(&sec, 0, 0x43);
    sections_make(&sec, RC_STR("extra"));
    RC_CHECK_FALSE(sections_emission_changed(&sec));

    rc_arena_deinit(&arena);
}

RC_TEST(sections, emit_org_reset)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);   // builds the default section at index 0

    sections_emit_u8(&sec, 0, 0xA9);
    RC_CHECK(sections_pc(&sec, 0), ==, 1u);
    RC_CHECK(sections_code(&sec, 0).num, ==, 1u);

    sections_emit_u16(&sec, 0, 0x1234);          // little-endian
    RC_CHECK(sections_pc(&sec, 0), ==, 3u);
    RC_CHECK((uint32_t)rc_view_bytes_get(sections_code(&sec, 0), 1), ==, 0x34u);
    RC_CHECK((uint32_t)rc_view_bytes_get(sections_code(&sec, 0), 2), ==, 0x12u);

    // A new address (org) moves pc only; the next byte still lands at code index 3. A full 32-bit
    // host address keeps its 6502 half.
    sections_org(&sec, 0, 0xFFFF2000);
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

RC_TEST(sections, guard_field)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // Every section starts unguarded - the default and named ones alike.
    RC_CHECK_FALSE(sections_is_guarded(&sec, 0));
    uint32_t s = sections_make(&sec, RC_STR("code"));
    RC_CHECK_FALSE(sections_is_guarded(&sec, s));

    // Setting one takes a full 32-bit host address and keeps the 6502's 16 bits.
    sections_set_guard(&sec, s, 0xFFFF3000);
    RC_CHECK_TRUE(sections_is_guarded(&sec, s));
    RC_CHECK(sections_guard(&sec, s), ==, 0x3000u);

    rc_arena_deinit(&arena);
}

RC_TEST(sections, make_unique_and_attributes)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
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

RC_TEST(sections, close_folds_child_into_parent)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // Parent at &2000 emits one byte, then a child rephased to &400 emits two, then the parent one
    // more: closing folds the child's window and size into the parent, so the parent's code carries
    // all four bytes in order and its pc advanced through the child's.
    uint32_t parent = sections_make(&sec, RC_STR("outer"));
    sections_org(&sec, parent, 0x2000);
    sections_emit_u8(&sec, parent, 0x01);
    uint32_t child = sections_make(&sec, RC_STR("inner"));
    sections_org(&sec, child, 0x400);
    sections_emit_u8(&sec, child, 0x02);
    sections_emit_u8(&sec, child, 0x03);
    sections_close(&sec, child, parent);
    sections_emit_u8(&sec, parent, 0x04);
    sections_close(&sec, parent, sections_default);

    RC_CHECK(sections_pc(&sec, child), ==, 0x402u);
    RC_CHECK(sections_pc(&sec, parent), ==, 0x2004u);
    RC_CHECK(sections_code(&sec, child).num, ==, 2u);
    RC_CHECK(sections_code(&sec, parent).num, ==, 4u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, parent), 1), ==, 0x02u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, parent), 3), ==, 0x04u);

    // The default is the root: its window covers the whole stream, its pc advanced with everything
    RC_CHECK(sections_pc(&sec, sections_default), ==, 4u);
    RC_CHECK(sections_code(&sec, sections_default).num, ==, 4u);

    rc_arena_deinit(&arena);
}

RC_TEST(sections, seal_slices_the_stream)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    sections_emit_u8(&sec, sections_default, 0xEA);
    uint32_t id = sections_make(&sec, RC_STR("code"));
    sections_emit_u8(&sec, id, 0xA9);
    sections_emit_u8(&sec, id, 0x2A);
    sections_close(&sec, id, sections_default);
    sections_seal(&sec);

    // The sealed views are slices of one stream: the named window inside the default's
    section root = rc_array_section_get(&sec.nodes, sections_default);
    section code = rc_array_section_get(&sec.nodes, id);
    RC_CHECK(root.code.num, ==, 3u);
    RC_CHECK(code.code.num, ==, 2u);
    RC_CHECK((uint32_t) rc_view_bytes_get(root.code, 0), ==, 0xEAu);
    RC_CHECK((uint32_t) rc_view_bytes_get(code.code, 0), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_view_bytes_get(code.code, 1), ==, 0x2Au);

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
    sections_init(&sec, &original, &original);
    sections_reset(&sec);
    uint32_t id = sections_make(&sec, name);
    sections_org(&sec, id, 0x1900);
    sections_emit_u8(&sec, id, 0xA9);
    sections_emit_u8(&sec, id, 0x2A);
    sections_add_attribute(&sec, id, key, value_make_string(file), cursor_none());
    sections_seal(&sec);

    section copy = section_make_copy(rc_array_section_get(&sec.nodes, id), &kept);

    // The decisive move: kill the arena everything was built in. If the copy borrowed any of it, the reads
    // below touch freed memory (which the sanitizer build turns into a hard failure).
    rc_arena_deinit(&original);

    RC_CHECK(copy.name, ==, RC_STR("code"));
    RC_CHECK(copy.pc, ==, 0x1902u);
    RC_CHECK(copy.code.num, ==, 2u);
    RC_CHECK((uint32_t) rc_view_bytes_get(copy.code, 0), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_view_bytes_get(copy.code, 1), ==, 0x2Au);
    RC_CHECK(copy.attributes.num, ==, 1u);
    RC_CHECK(rc_array_attribute_get(&copy.attributes, 0).key, ==, RC_STR("filename"));
    RC_CHECK_TRUE(value_is_equal(rc_array_attribute_get(&copy.attributes, 0).v, value_make_string(RC_STR("game.bin"))));

    rc_arena_deinit(&kept);
}

#endif // BARON_TESTS
