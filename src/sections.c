#include "sections.h"

#include "richc/macros.h"
#include "richc/mstr.h"


enum {
    sections_nodes_reserve = 64,        // 64 ROM banks / sections is a very large project
    section_code_reserve   = 0x10000,   // 64 KB: the hard 6502 ceiling, so a code buffer never grows within a pass
    section_attrs_reserve  = 4,         // a handful of attributes per section is typical
};


// ---- lifecycle ----

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent)
{
    RC_ASSERT(sec != NULL && per_pass != NULL && permanent != NULL);

    // The node list stays empty here: sections_reset builds it at the top of each pass. sizes and
    // emissions are lazily made and deliberately survive the per-pass resets.
    sec->arena     = per_pass;
    sec->permanent = permanent;
    sec->nodes     = (rc_array_section) {0};
    sec->splices   = (rc_array_splice) {0};
    sec->sizes     = (rc_array_section_size) {0};
    sec->emissions = (rc_view_section_emission) {0};
}


void sections_reset(sections *sec)
{
    RC_ASSERT(sec != NULL);

    // The default section is index 0
    sec->nodes = rc_array_section_make(sections_nodes_reserve, sec->arena);
    rc_array_section_push(
        &sec->nodes,
        (section) { .guard = RC_INDEX_NONE,
                    .code = rc_array_bytes_make(section_code_reserve, sec->arena) },
        sec->arena);

    // This pass's splice records start empty; the arena behind the old ones was just reset
    sec->splices = (rc_array_splice) {0};
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
    return rc_array_section_get(&sec->nodes, id).code.view;
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


uint32_t sections_guard(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
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
        (section) { .name = name, .guard = RC_INDEX_NONE,
                    .code = rc_array_bytes_make(section_code_reserve, sec->arena) },
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

    // Only the effective address moves; code still appends at code.num
    rc_array_section_at(&sec->nodes, id)->pc = addr;
}


void sections_set_cmos(sections *sec, uint32_t id, bool cmos)
{
    RC_ASSERT(sec != NULL);
    rc_array_section_at(&sec->nodes, id)->cmos = cmos;
}


void sections_set_guard(sections *sec, uint32_t id, uint32_t addr)
{
    RC_ASSERT(sec != NULL);
    rc_array_section_at(&sec->nodes, id)->guard = addr;
}


void sections_emit_u8(sections *sec, uint32_t id, uint8_t b)
{
    RC_ASSERT(sec != NULL);

    section *s = rc_array_section_at(&sec->nodes, id);
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

    section *s = rc_array_section_at(&sec->nodes, id);
    for (uint32_t i = 0; i < count; i++) {
        rc_array_bytes_push(&s->code, 0, sec->arena);
    }

    s->pc += count;
}


// ---- splices (INCSECTION) ----

// The size the named section ended the LAST pass with, or 0 when it has never been seen - the best a
// splice can reserve for a source not built yet this pass.
static uint32_t prior_size(const sections *sec, rc_str name)
{
    for (uint32_t i = 0; i < sec->sizes.num; i++) {
        section_size e = rc_array_section_size_get(&sec->sizes, i);
        if (rc_str_is_equal(e.name, name)) {
            return e.size;
        }
    }

    return 0;
}


void sections_splice(sections *sec, uint32_t dst, rc_str src_name, cursor at)
{
    RC_ASSERT(sec != NULL);

    // Reserve the source's best-known size as zeroes: its live size if built this pass, else last
    // pass's size, else 0 on a first sighting (the settle check demands another pass once known). A
    // still-open source reserves its partial size - harmless, that shape is a cycle and fails the pass.
    uint32_t src    = sections_find(sec, src_name);
    uint32_t count  = (src != RC_INDEX_NONE) ? sections_code(sec, src).num : prior_size(sec, src_name);
    uint32_t offset = sections_code(sec, dst).num;

    sections_skip(sec, dst, count);

    if (sec->splices.data == NULL) {
        sec->splices = rc_array_splice_make(8, sec->arena);
    }

    rc_array_splice_push(
        &sec->splices,
        (splice) {
            .dst        = dst,
            .dst_offset = offset,
            .src_name   = src_name,
            .count      = count,
            .at         = at,
        },
        sec->arena);
}


rc_view_splice sections_splices(const sections *sec)
{
    RC_ASSERT(sec != NULL);
    return sec->splices.view;
}


bool sections_splices_changed(const sections *sec)
{
    RC_ASSERT(sec != NULL);

    for (uint32_t i = 0; i < sec->splices.num; i++) {
        splice sp  = rc_array_splice_get(&sec->splices, i);
        uint32_t src = sections_find(sec, sp.src_name);
        uint32_t now = (src != RC_INDEX_NONE) ? sections_code(sec, src).num : 0;
        if (now != sp.count) {
            return true;   // the reservation missed: the layout will differ next pass
        }
    }

    return false;
}


void sections_copy_in(sections *sec, uint32_t dst, uint32_t offset, uint32_t src)
{
    RC_ASSERT(sec != NULL && dst != src);

    section *d = rc_array_section_at(&sec->nodes, dst);
    rc_view_bytes s = sections_code(sec, src);
    RC_ASSERT((uint64_t) offset + s.num <= d->code.num);   // the span was reserved at exactly this size

    for (uint32_t i = 0; i < s.num; i++) {
        rc_array_bytes_set(&d->code, offset + i, rc_view_bytes_get(s, i));
    }
}


// ---- cross-pass bookkeeping ----

// Record (or refresh) the named section's cross-pass size. The name is a view into permanent source text,
// so storing it outlives every pass; the entries themselves live in the permanent arena for the same reason.
static void size_upsert(sections *sec, rc_str name, uint32_t size)
{
    for (uint32_t i = 0; i < sec->sizes.num; i++) {
        if (rc_str_is_equal(rc_array_section_size_get(&sec->sizes, i).name, name)) {
            rc_array_section_size_at(&sec->sizes, i)->size = size;
            return;
        }
    }

    if (sec->sizes.data == NULL) {
        sec->sizes = rc_array_section_size_make(8, sec->permanent);
    }

    rc_array_section_size_push(&sec->sizes, (section_size) {.name = name, .size = size}, sec->permanent);
}


void sections_note_sizes(sections *sec)
{
    RC_ASSERT(sec != NULL);

    for (uint32_t i = 0; i < sec->nodes.num; i++) {
        section s = rc_array_section_get(&sec->nodes, i);
        if (s.name.len != 0) {
            size_upsert(sec, s.name, s.code.num);
        }
    }

    // A splice source that never appeared this pass is pinned at 0, so a section that VANISHED across
    // passes (an IF arm flipping) cannot leave a stale size re-reserving itself forever
    for (uint32_t i = 0; i < sec->splices.num; i++) {
        rc_str name = rc_array_splice_get(&sec->splices, i).src_name;
        if (sections_find(sec, name) == RC_INDEX_NONE) {
            size_upsert(sec, name, 0);
        }
    }
}


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

    // Compare each section's fingerprint against last time's while rebuilding the noted set for next time
    rc_array_section_emission next = rc_array_section_emission_make(sec->nodes.num, sec->permanent);
    for (uint32_t i = 0; i < sec->nodes.num; i++) {
        section_emission e = {
            .size = rc_array_section_get(&sec->nodes, i).code.num,
            .crc  = crc32_bytes(rc_array_section_get(&sec->nodes, i).code.view),
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

    return (section) {
        .name       = str_make_copy(s.name, arena),
        .pc         = s.pc,
        .cmos       = s.cmos,
        .guard      = s.guard,
        .code       = rc_array_bytes_make_copy(s.code.view, 0, arena),
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

RC_TEST(sections, guard_field)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // Every section starts unguarded - the default and named ones alike.
    RC_CHECK(sections_guard(&sec, 0), ==, RC_INDEX_NONE);
    uint32_t s = sections_make(&sec, RC_STR("code"));
    RC_CHECK(sections_guard(&sec, s), ==, RC_INDEX_NONE);
    sections_set_guard(&sec, s, 0x3000);
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

RC_TEST(sections, splice_reserves_settles_and_copies)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // Pass 1, forward order: load splices "code" before code exists - nothing is known, so nothing is
    // reserved - then code appears with three bytes. The settle check must demand another pass.
    uint32_t load = sections_make(&sec, RC_STR("load"));
    sections_splice(&sec, load, RC_STR("code"), (cursor) {0});
    RC_CHECK(sections_code(&sec, load).num, ==, 0u);
    uint32_t code = sections_make(&sec, RC_STR("code"));
    RC_CHECK(sections_find(&sec, RC_STR("code")), ==, code);
    sections_emit_u8(&sec, code, 0xA9);
    sections_emit_u8(&sec, code, 0x2A);
    sections_emit_u8(&sec, code, 0x60);
    RC_CHECK_TRUE(sections_splices_changed(&sec));   // reserved 0, the source settled at 3
    sections_note_sizes(&sec);

    // Pass 2: the forward splice now reserves last pass's size as ZEROES (the copy comes later), and the
    // settle check is content.
    sections_reset(&sec);
    load = sections_make(&sec, RC_STR("load"));
    sections_emit_u8(&sec, load, 0xEA);                       // a stub byte ahead of the splice
    sections_splice(&sec, load, RC_STR("code"), (cursor) {0});
    code = sections_make(&sec, RC_STR("code"));
    sections_emit_u8(&sec, code, 0xA9);
    sections_emit_u8(&sec, code, 0x2A);
    sections_emit_u8(&sec, code, 0x60);
    RC_CHECK_FALSE(sections_splices_changed(&sec));
    RC_CHECK(sections_code(&sec, load).num, ==, 4u);          // stub + 3 reserved
    RC_CHECK(sections_pc(&sec, load), ==, 4u);                // pc advanced with the reservation
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, load), 1), ==, 0x00u);   // still a hole

    // The fixup copy overwrites the reserved span in place; pc and length are untouched.
    splice sp = rc_view_splice_get(sections_splices(&sec), 0);
    RC_CHECK(sp.dst_offset, ==, 1u);
    sections_copy_in(&sec, sp.dst, sp.dst_offset, sections_find(&sec, sp.src_name));
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, load), 0), ==, 0xEAu);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, load), 1), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, load), 2), ==, 0x2Au);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, load), 3), ==, 0x60u);
    RC_CHECK(sections_code(&sec, load).num, ==, 4u);

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

    section copy = section_make_copy(rc_array_section_get(&sec.nodes, id), &kept);

    // The decisive move: kill the arena everything was built in. If the copy borrowed any of it, the reads
    // below touch freed memory (which the sanitizer build turns into a hard failure).
    rc_arena_deinit(&original);

    RC_CHECK(copy.name, ==, RC_STR("code"));
    RC_CHECK(copy.pc, ==, 0x1902u);
    RC_CHECK(copy.code.num, ==, 2u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 0), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&copy.code, 1), ==, 0x2Au);
    RC_CHECK(copy.attributes.num, ==, 1u);
    RC_CHECK(rc_array_attribute_get(&copy.attributes, 0).key, ==, RC_STR("filename"));
    RC_CHECK_TRUE(value_is_equal(rc_array_attribute_get(&copy.attributes, 0).v, value_make_string(RC_STR("game.bin"))));

    rc_arena_deinit(&kept);
}

#endif // BARON_TESTS
