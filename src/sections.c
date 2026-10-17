#include "sections.h"

#include "richc/macros.h"
#include "richc/mstr.h"


enum {
    sections_nodes_reserve = 64,    // 64 ROM banks / sections is a very large project
    section_code_floor     = 256,   // the first pass's opening buffer, before there is a size to go on
    section_attrs_reserve  = 4,     // a handful of attributes per section is typical
};


// ---- lifecycle ----

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent)
{
    RC_ASSERT(sec != NULL && per_pass != NULL && permanent != NULL);

    // The node list stays empty here: sections_reset builds it at the top of each pass. The emission
    // fingerprints are lazily made and deliberately survive the per-pass resets.
    sec->arena     = per_pass;
    sec->permanent = permanent;
    sec->nodes     = (rc_array_section) {0};
    sec->emissions = (rc_view_section_emission) {0};
}


void sections_reset(sections *sec)
{
    RC_ASSERT(sec != NULL);

    // Just the default section at index 0, which is open for the whole pass
    sec->nodes = rc_array_section_make(sections_nodes_reserve, sec->arena);
    rc_array_section_push(&sec->nodes, (section) {.parent = RC_INDEX_NONE, .is_open = true}, sec->arena);
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


bool sections_is_virtual(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return rc_array_section_get(&sec->nodes, id).is_virtual;
}


bool sections_has_org(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);

    // The key alone decides, resolved or not, so the answer is the same on every pass
    rc_view_attribute attrs = sections_attributes(sec, id);
    for (uint32_t i = 0; i < attrs.num; i++) {
        if (rc_str_is_equal_insensitive(rc_view_attribute_get(attrs, i).key, RC_STR("org"))) {
            return true;
        }
    }

    return false;
}


bool sections_is_open(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);
    return rc_array_section_get(&sec->nodes, id).is_open;
}


bool sections_can_reopen(const sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);

    section s = rc_array_section_get(&sec->nodes, id);
    return s.reopenable && !s.is_open;
}


// ---- mutation ----

uint32_t sections_make(sections *sec, rc_str name, uint32_t parent)
{
    RC_ASSERT(sec != NULL && parent < sec->nodes.num);

    // A repeated name is a duplicate: hand back RC_INDEX_NONE and let the caller diagnose it
    if (sections_find(sec, name) != RC_INDEX_NONE) {
        return RC_INDEX_NONE;
    }

    // The pc is copied whole, not through sections_org's 16-bit mask: a parent filled right to &10000
    // must not hand its child address 0. The code buffer waits for the first byte (see make_room).
    return rc_array_section_push(
        &sec->nodes,
        (section) {
            .name    = name,
            .parent  = parent,
            .pc      = sections_pc(sec, parent),
            .is_open = true,
        },
        sec->arena);
}


void sections_add_attribute(sections *sec, uint32_t id, rc_str key, value v, cursor at)
{
    RC_ASSERT(sec != NULL);

    section *s = rc_array_section_at(&sec->nodes, id);
    value copy = value_make_copy(v, sec->arena);   // durable backing in the (per-pass) manager arena

    // Upsert: a key repeated on one SECTION line is replaced in place, last wins
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


void sections_set_virtual(sections *sec, uint32_t id, bool is_virtual)
{
    RC_ASSERT(sec != NULL && id != sections_default);
    rc_array_section_at(&sec->nodes, id)->is_virtual = is_virtual;
}


// Move s's pc on by count, noticing the step that first goes past &FFFF (the subtraction cannot
// underflow on that side of the test, nor the addition wrap on a huge count).
static void advance(section *s, uint32_t count)
{
    if (s->pc <= sections_address_space && count > sections_address_space - s->pc) {
        s->overrun = true;
    }
    s->size += count;
    s->pc   += count;
}


// Make sure section id's buffer exists before its first n bytes land. It opens at last pass's final
// size (the fingerprints remember it), so once the layout has settled a buffer is allocated exactly
// once and never moves - sections growing side by side in one arena would otherwise keep relocating
// past each other. After that, ordinary doubling growth takes over.
static void make_room(sections *sec, section *s, uint32_t id, uint32_t n)
{
    if (s->code.data != NULL) {
        return;
    }

    uint32_t hint = id < sec->emissions.num ? rc_view_section_emission_get(sec->emissions, id).size : 0;
    uint32_t cap  = hint > section_code_floor ? hint : section_code_floor;
    s->code = rc_array_bytes_make(cap > n ? cap : n, sec->arena);
}


void sections_emit_u8(sections *sec, uint32_t id, uint8_t b)
{
    RC_ASSERT(sec != NULL);

    // A virtual section keeps nothing: the byte only claims its address
    section *s = rc_array_section_at(&sec->nodes, id);
    if (!s->is_virtual) {
        make_room(sec, s, id, 1);
        rc_array_bytes_push(&s->code, b, sec->arena);
    }
    advance(s, 1);
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
    if (!s->is_virtual) {
        make_room(sec, s, id, count);
        rc_array_bytes_push_n_zero(&s->code, count, sec->arena);
    }
    advance(s, count);
}


void sections_set_u8(sections *sec, uint32_t id, uint32_t offset, uint8_t b)
{
    RC_ASSERT(sec != NULL);

    section *s = rc_array_section_at(&sec->nodes, id);
    RC_ASSERT(!s->is_virtual);
    rc_array_bytes_set(&s->code, offset, b);
}


bool sections_take_overrun(sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL);

    section *s = rc_array_section_at(&sec->nodes, id);
    bool overrun = s->overrun;
    s->overrun = false;
    return overrun;
}


void sections_reopen(sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL && sections_can_reopen(sec, id));
    rc_array_section_at(&sec->nodes, id)->is_open = true;
}


void sections_close(sections *sec, uint32_t id)
{
    RC_ASSERT(sec != NULL && id != sections_default && sections_is_open(sec, id));

    section *c = rc_array_section_at(&sec->nodes, id);
    c->is_open = false;

    // The cases that leave the parent alone (see the header's table), which is what makes a section safe
    // to write again: there is no room in the parent for a later piece to have to fit into.
    bool own_org = sections_has_org(sec, id);
    section *p   = rc_array_section_at(&sec->nodes, c->parent);
    if (c->parent == sections_default || (p->is_virtual ? own_org : c->is_virtual)) {
        c->reopenable = true;
        return;
    }

    // A child that continued our addresses already reported going past &FFFF itself; one with its own
    // org only shows up here, as our pc moves past its bytes
    bool overrun = p->overrun;
    advance(p, c->size);
    if (!own_org) {
        p->overrun = overrun;
    }
    if (!p->is_virtual && c->code.num != 0) {
        make_room(sec, p, c->parent, c->code.num);
        rc_array_bytes_append(&p->code, c->code.view, sec->arena);
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
    // time. A parent's bytes include its children's copies, so a child's shift also fingerprints through
    // its ancestors - redundant, but only ever MORE eager, and index order is pass-stable.
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

    return (section) {
        .name       = str_make_copy(s.name, arena),
        .parent     = s.parent,
        .pc         = s.pc,
        .size       = s.size,
        .cmos       = s.cmos,
        .is_virtual = s.is_virtual,
        .is_guarded = s.is_guarded,
        .is_open    = s.is_open,
        .reopenable = s.reopenable,
        .overrun    = s.overrun,
        .guard      = s.guard,
        .code       = rc_array_bytes_make_copy(s.code.view, 0, arena),
        .attributes = attributes,
    };
}


#ifdef BARON_TESTS

#include "richc/test.h"

// An org as handle_section writes one: the attribute (which makes it the section's own) and the pc
static void test_org(sections *sec, uint32_t id, uint32_t addr)
{
    sections_add_attribute(sec, id, RC_STR("org"), value_make_numeric(addr), cursor_none());
    sections_org(sec, id, addr);
}

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
    sections_make(&sec, RC_STR("extra"), sections_default);
    RC_CHECK_TRUE(sections_emission_changed(&sec));

    // Pass 5, stable again.
    sections_reset(&sec);
    sections_emit_u8(&sec, 0, 0xA9);
    sections_emit_u8(&sec, 0, 0x43);
    sections_make(&sec, RC_STR("extra"), sections_default);
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

RC_TEST(sections, set_u8_overwrites_own_bytes)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // The offset counts from the section's own bytes: a child opened after two bytes of its parent
    // overwrites its own first byte, and neither pc moves.
    sections_emit_u16(&sec, 0, 0x1111);
    uint32_t child = sections_make(&sec, RC_STR("child"), 0);
    sections_emit_u8(&sec, child, 0x00);
    sections_emit_u8(&sec, child, 0x22);
    sections_set_u8(&sec, child, 0, 0x33);
    RC_CHECK(sections_pc(&sec, child), ==, 4u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, child), 0), ==, 0x33u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, 0), 0), ==, 0x11u);

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
    uint32_t s = sections_make(&sec, RC_STR("code"), sections_default);
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

    uint32_t a = sections_make(&sec, RC_STR("code"), sections_default);
    RC_CHECK(a, ==, 1u);                                  // first named section after the default
    uint32_t d = sections_make(&sec, RC_STR("data"), sections_default);
    RC_CHECK(d, ==, 2u);
    RC_CHECK_TRUE(sections_make(&sec, RC_STR("code"), sections_default) == RC_INDEX_NONE);   // a repeat is refused

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
    // more: closing copies the child's bytes onto the parent's, so the parent's code carries all four
    // in order and its pc moved past the child's.
    uint32_t parent = sections_make(&sec, RC_STR("outer"), sections_default);
    test_org(&sec, parent, 0x2000);
    sections_emit_u8(&sec, parent, 0x01);
    uint32_t child = sections_make(&sec, RC_STR("inner"), parent);
    test_org(&sec, child, 0x400);
    sections_emit_u8(&sec, child, 0x02);
    sections_emit_u8(&sec, child, 0x03);
    sections_close(&sec, child);
    sections_emit_u8(&sec, parent, 0x04);
    sections_close(&sec, parent);

    RC_CHECK(rc_array_section_get(&sec.nodes, child).parent, ==, parent);
    RC_CHECK(rc_array_section_get(&sec.nodes, parent).parent, ==, (uint32_t) sections_default);
    RC_CHECK(rc_array_section_get(&sec.nodes, sections_default).parent, ==, RC_INDEX_NONE);
    RC_CHECK(sections_pc(&sec, child), ==, 0x402u);
    RC_CHECK(sections_pc(&sec, parent), ==, 0x2004u);
    RC_CHECK(sections_code(&sec, child).num, ==, 2u);
    RC_CHECK(sections_code(&sec, parent).num, ==, 4u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, parent), 1), ==, 0x02u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, parent), 3), ==, 0x04u);

    // The nested child took room in its parent, so it can never be written again; the top-level parent
    // stood alone - the default holds none of it - and can
    RC_CHECK_FALSE(sections_can_reopen(&sec, child));
    RC_CHECK_TRUE(sections_can_reopen(&sec, parent));
    RC_CHECK(sections_pc(&sec, sections_default), ==, 0u);
    RC_CHECK(sections_code(&sec, sections_default).num, ==, 0u);

    rc_arena_deinit(&arena);
}

RC_TEST(sections, close_rules_for_virtual_sections)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // A virtual child of a real parent (with the org it needs) keeps nothing and leaves the parent alone
    uint32_t code = sections_make(&sec, RC_STR("code"), sections_default);
    test_org(&sec, code, 0x2000);
    sections_emit_u8(&sec, code, 0x01);
    uint32_t ws = sections_make(&sec, RC_STR("ws"), code);
    sections_set_virtual(&sec, ws, true);
    test_org(&sec, ws, 0x400);
    sections_emit_u16(&sec, ws, 0x1234);
    sections_skip(&sec, ws, 3);
    sections_close(&sec, ws);
    RC_CHECK(sections_pc(&sec, ws), ==, 0x405u);
    RC_CHECK(sections_code(&sec, ws).num, ==, 0u);
    RC_CHECK(sections_pc(&sec, code), ==, 0x2001u);
    RC_CHECK_TRUE(sections_can_reopen(&sec, ws));

    // A virtual parent is a memory map: a real child continuing its addresses keeps its own bytes, and
    // the map moves past them without keeping any
    uint32_t map = sections_make(&sec, RC_STR("map"), sections_default);
    sections_set_virtual(&sec, map, true);
    test_org(&sec, map, 0x1900);
    uint32_t file = sections_make(&sec, RC_STR("file"), map);
    sections_emit_u8(&sec, file, 0xEA);
    sections_emit_u8(&sec, file, 0x60);
    sections_close(&sec, file);
    RC_CHECK(sections_code(&sec, file).num, ==, 2u);
    RC_CHECK(sections_code(&sec, map).num, ==, 0u);
    RC_CHECK(sections_pc(&sec, map), ==, 0x1902u);
    RC_CHECK_FALSE(sections_can_reopen(&sec, file));

    // So does a virtual child continuing its addresses (a group within the map)...
    uint32_t group = sections_make(&sec, RC_STR("group"), map);
    sections_set_virtual(&sec, group, true);
    sections_skip(&sec, group, 0x10);
    sections_close(&sec, group);
    RC_CHECK(sections_pc(&sec, map), ==, 0x1912u);
    RC_CHECK_FALSE(sections_can_reopen(&sec, group));

    // ...but a child with an org of its own, real or virtual, is placed elsewhere and takes no room
    uint32_t zp = sections_make(&sec, RC_STR("zp"), map);
    sections_set_virtual(&sec, zp, true);
    test_org(&sec, zp, 0x70);
    sections_skip(&sec, zp, 2);
    sections_close(&sec, zp);
    uint32_t far = sections_make(&sec, RC_STR("far"), map);
    test_org(&sec, far, 0x8000);
    sections_emit_u8(&sec, far, 0x42);
    sections_close(&sec, far);
    RC_CHECK(sections_pc(&sec, map), ==, 0x1912u);
    RC_CHECK(sections_code(&sec, far).num, ==, 1u);
    RC_CHECK_TRUE(sections_can_reopen(&sec, zp));
    RC_CHECK_TRUE(sections_can_reopen(&sec, far));

    rc_arena_deinit(&arena);
}

RC_TEST(sections, reopen_carries_on)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // A section cannot be reopened while it is open; once closed, a second piece joins the first
    uint32_t a = sections_make(&sec, RC_STR("a"), sections_default);
    test_org(&sec, a, 0x3000);
    RC_CHECK_TRUE(sections_is_open(&sec, a));
    RC_CHECK_FALSE(sections_can_reopen(&sec, a));
    sections_emit_u8(&sec, a, 0x01);
    sections_close(&sec, a);
    RC_CHECK_FALSE(sections_is_open(&sec, a));

    sections_emit_u8(&sec, sections_default, 0xFF);   // written between the pieces, and not part of a
    sections_reopen(&sec, a);
    RC_CHECK_TRUE(sections_is_open(&sec, a));
    sections_emit_u8(&sec, a, 0x02);
    sections_close(&sec, a);

    RC_CHECK(sections_pc(&sec, a), ==, 0x3002u);
    RC_CHECK(sections_code(&sec, a).num, ==, 2u);
    RC_CHECK((uint32_t) rc_view_bytes_get(sections_code(&sec, a), 1), ==, 0x02u);
    RC_CHECK(sections_code(&sec, sections_default).num, ==, 1u);

    rc_arena_deinit(&arena);
}

RC_TEST(sections, buffers_open_at_last_pass_size)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);

    // Pass 1 has nothing to go on, so a big section grows its way there
    sections_reset(&sec);
    uint32_t big = sections_make(&sec, RC_STR("big"), sections_default);
    sections_skip(&sec, big, 5000);
    sections_emission_changed(&sec);

    // Pass 2 opens the buffer at that size on the first byte, so it never has to move
    sections_reset(&sec);
    big = sections_make(&sec, RC_STR("big"), sections_default);
    sections_emit_u8(&sec, big, 0xEA);
    RC_CHECK(rc_array_section_get(&sec.nodes, big).code.cap, >=, 5000u);

    // A section that keeps nothing never allocates at all
    uint32_t ws = sections_make(&sec, RC_STR("ws"), sections_default);
    sections_set_virtual(&sec, ws, true);
    sections_skip(&sec, ws, 100);
    RC_CHECK_TRUE(rc_array_section_get(&sec.nodes, ws).code.data == NULL);

    rc_arena_deinit(&arena);
}

RC_TEST(sections, overrun_fires_once)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // Reaching &10000 exactly is no overrun; the next byte is, and asking again finds it already taken.
    uint32_t a = sections_make(&sec, RC_STR("a"), sections_default);
    sections_org(&sec, a, 0xFFFE);
    sections_emit_u16(&sec, a, 0x1234);
    RC_CHECK_FALSE(sections_take_overrun(&sec, a));
    sections_emit_u8(&sec, a, 0x56);
    RC_CHECK_TRUE(sections_take_overrun(&sec, a));
    RC_CHECK_FALSE(sections_take_overrun(&sec, a));

    // Already past the top, nothing crosses again - and a child continues the parent's whole pc rather
    // than its low 16 bits, so it does not quietly restart at address 5.
    sections_skip(&sec, a, 4);
    uint32_t b = sections_make(&sec, RC_STR("b"), a);
    RC_CHECK(sections_pc(&sec, b), ==, 0x10005u);
    sections_emit_u8(&sec, b, 0x78);
    RC_CHECK_FALSE(sections_take_overrun(&sec, b));

    // A child with its own org fits its own addresses, but closing it pushes the parent past the top:
    // that crossing is the parent's to report
    uint32_t c = sections_make(&sec, RC_STR("c"), sections_default);
    test_org(&sec, c, 0xFF00);
    uint32_t d = sections_make(&sec, RC_STR("d"), c);
    test_org(&sec, d, 0x1000);
    sections_skip(&sec, d, 0x200);
    sections_close(&sec, d);
    RC_CHECK(sections_pc(&sec, c), ==, 0x10100u);
    RC_CHECK_FALSE(sections_take_overrun(&sec, d));
    RC_CHECK_TRUE(sections_take_overrun(&sec, c));

    // A child continuing the parent's addresses reports its own crossing, so its close stays quiet
    uint32_t e = sections_make(&sec, RC_STR("e"), sections_default);
    test_org(&sec, e, 0xFFFF);
    uint32_t f = sections_make(&sec, RC_STR("f"), e);
    sections_skip(&sec, f, 2);
    RC_CHECK_TRUE(sections_take_overrun(&sec, f));
    sections_close(&sec, f);
    RC_CHECK_FALSE(sections_take_overrun(&sec, e));

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
    uint32_t id = sections_make(&sec, name, sections_default);
    sections_org(&sec, id, 0x1900);
    sections_emit_u8(&sec, id, 0xA9);
    sections_emit_u8(&sec, id, 0x2A);
    sections_add_attribute(&sec, id, key, value_make_string(file), cursor_none());

    section copy = section_make_copy(rc_array_section_get(&sec.nodes, id), &kept);

    // The decisive move: kill the arena everything was built in. If the copy borrowed any of it, the reads
    // below touch freed memory (which the sanitizer build turns into a hard failure).
    rc_arena_deinit(&original);

    RC_CHECK(copy.name, ==, RC_STR("code"));
    RC_CHECK(copy.parent, ==, (uint32_t) sections_default);
    RC_CHECK(copy.pc, ==, 0x1902u);
    RC_CHECK(copy.code.num, ==, 2u);
    RC_CHECK((uint32_t) rc_view_bytes_get(copy.code.view, 0), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_view_bytes_get(copy.code.view, 1), ==, 0x2Au);
    RC_CHECK(copy.attributes.num, ==, 1u);
    RC_CHECK(rc_array_attribute_get(&copy.attributes, 0).key, ==, RC_STR("filename"));
    RC_CHECK_TRUE(value_is_equal(rc_array_attribute_get(&copy.attributes, 0).v, value_make_string(RC_STR("game.bin"))));

    rc_arena_deinit(&kept);
}

#endif // BARON_TESTS
