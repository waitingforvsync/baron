#ifndef BARON_SECTIONS_H_
#define BARON_SECTIONS_H_

#include "richc/bytes.h"
#include "value.h"
#include "cursor.h"


// ---- types ----

// One key = expr pair off the SECTION line, resolved to a value at assembly time. The assembler
// consumes the keys it knows (org, cmos, ...); the rest ride here for the output stage to read.
typedef struct attribute {
    rc_str key;   // view into the source text (permanent)
    value  v;     // copied into the manager arena, so its backing is durable
    cursor at;    // where the pair was written, for diagnostics
} attribute;

#define RC_ARRAY_TYPE attribute
#define RC_ARRAY_NAME attribute
#include "richc/template/array.h"


// One section: a contiguous block of object code with its own pc, a unique name and a resolved
// attribute bag. The byte array fills from index 0 independent of pc, so an explicit org
// repositions later labels without moving where code lands.
typedef struct section {
    rc_str              name;         // own namespace, separate from symbols and scopes; a view into source text
    uint32_t            pc;           // effective address of the next byte - the value a label takes
    bool                cmos;         // consumed cmos attribute: 65C02 encodings allowed here
    uint32_t            guard;        // consumed guard attribute: first address emission must not reach (RC_INDEX_NONE = unguarded)
    rc_array_bytes      code;
    rc_array_attribute  attributes;
} section;

#define RC_ARRAY_TYPE section
#define RC_ARRAY_NAME section
#include "richc/template/array.h"


// One recorded INCSECTION: count zero bytes reserved at dst_offset in section dst, to be
// overwritten with the bytes of the section named src_name by the post-assembly fixup. The copy
// must wait until then: the source may be defined later, and the zp allocator patches bytes after
// the final pass - copying last, in dependency order, gets both right.
typedef struct splice {
    uint32_t dst;          // destination section index
    uint32_t dst_offset;   // where in dst's code buffer the reserved span begins
    rc_str   src_name;     // the named source section (a view into permanent source text)
    uint32_t count;        // bytes reserved this pass - the source's best-known size
    cursor   at;           // the INCSECTION statement, for diagnostics
} splice;

#define RC_ARRAY_TYPE splice
#define RC_ARRAY_NAME splice
#include "richc/template/array.h"


// A section's size as it stood at the end of the last pass - what a splice reserves for a source not
// (yet) built this pass. Lives in the permanent arena: it is precisely cross-pass memory.
typedef struct section_size {
    rc_str   name;
    uint32_t size;
} section_size;

#define RC_ARRAY_TYPE section_size
#define RC_ARRAY_NAME section_size
#include "richc/template/array.h"


// One section's emission fingerprint at the end of a pass: enough to notice the content changing
// between settling passes even when no symbol moved (see sections_emission_changed).
typedef struct section_emission {
    uint32_t size;
    uint32_t crc;
} section_emission;

#define RC_ARRAY_TYPE section_emission
#define RC_ARRAY_NAME section_emission
#include "richc/template/array.h"


// The section manager: sections addressed by index, in the same spirit as scopes is for symbols.
// Sections are per-pass (object code is only read from the final pass's output), rebuilt by
// sections_reset each pass; only sizes and emissions survive, in the permanent arena.
typedef struct sections {
    rc_arena                  *arena;       // borrowed: baron's per_pass arena - nodes, code, attributes, splices
    rc_arena                  *permanent;   // borrowed: backs sizes + emissions, the only cross-pass state
    rc_array_section           nodes;       // index 0 is the default section
    rc_array_splice            splices;     // this pass's INCSECTIONs, in statement order
    rc_array_section_size      sizes;       // name -> size at the end of the last pass
    rc_view_section_emission   emissions;   // per-section {size, crc}, rebuilt in full each settling pass
} sections;

enum { sections_default = 0 };   // index of the default section


// ---- lifecycle ----

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent);

// Rebuild the list for a fresh pass: just the nameless default section at index 0. Call at the top of
// each pass, after the per_pass arena has been reset (this also makes the default the first time round).
void sections_reset(sections *sec);


// ---- queries ----

uint32_t      sections_pc(const sections *sec, uint32_t id);
rc_view_bytes sections_code(const sections *sec, uint32_t id);

// The whole section list as a read-only view (index 0 is the default) - what a baron_result hands back.
rc_view_section sections_all(const sections *sec);

// The resolved attribute bag of section id (empty if it has none).
rc_view_attribute sections_attributes(const sections *sec, uint32_t id);

// The index of the section named name, or RC_INDEX_NONE. A linear scan (sections are few) skipping
// the nameless default.
uint32_t sections_find(const sections *sec, rc_str name);

// Are 65C02 encodings allowed in section id (the consumed cmos attribute)?
bool sections_cmos(const sections *sec, uint32_t id);

// Section id's guard address, or RC_INDEX_NONE if unguarded (the consumed guard attribute).
uint32_t sections_guard(const sections *sec, uint32_t id);


// ---- mutation ----

// Create the named section (own pc 0, empty code, empty attributes) and return its stable index, or
// RC_INDEX_NONE if the name already exists this pass (names are unique; the caller raises the error).
// Indices are stable across passes because creation order is first-sighting parse order, identical each pass.
uint32_t sections_make(sections *sec, rc_str name);

// Add (or, for an already-present key, replace) one attribute on section id. Upsert semantics carry the
// inherit-then-override rule: a nested section copies its parent's bag first, then its own keys replace.
void sections_add_attribute(sections *sec, uint32_t id, rc_str key, value v, cursor at);

// Set section id's pc; does not move code already emitted.
void sections_org(sections *sec, uint32_t id, uint32_t addr);

// Set the consumed cmos attribute: allow 65C02 encodings in section id.
void sections_set_cmos(sections *sec, uint32_t id, bool cmos);

// Set the consumed guard attribute: the first address emission in section id must not reach.
void sections_set_guard(sections *sec, uint32_t id, uint32_t addr);

// Append a byte, pc += 1.
void sections_emit_u8(sections *sec, uint32_t id, uint8_t b);

// Append a little-endian word, pc += 2.
void sections_emit_u16(sections *sec, uint32_t id, uint16_t w);

// Append count zero bytes, pc += count.
void sections_skip(sections *sec, uint32_t id, uint32_t count);


// ---- splices (INCSECTION) ----

// Record an INCSECTION: reserve the source's best-known size in dst as zero bytes (pc advances with
// them) and note the fixup for the post-assembly copy.
void sections_splice(sections *sec, uint32_t dst, rc_str src_name, cursor at);

// This pass's splice records, in statement order - the assembler's cycle check and final fixup walk them.
rc_view_splice sections_splices(const sections *sec);

// Did any splice reserve a size other than its source's settled size this pass? Folded into the pass's
// changed flag so the layout gets another pass. An unknown source compares against 0, so a genuinely
// missing section does not spin passes - the final fixup step reports it instead.
bool sections_splices_changed(const sections *sec);

// The fixup copy: overwrite dst's bytes at [offset, offset + src size) with src's whole code buffer.
// The span was reserved by sections_splice and the sizes have settled, so it fits exactly.
void sections_copy_in(sections *sec, uint32_t dst, uint32_t offset, uint32_t src);


// ---- cross-pass bookkeeping ----

// End-of-pass: record every named section's size into the cross-pass map (pinning a splice source that
// never appeared at 0, so a vanished section cannot leave a stale size spinning the convergence loop).
void sections_note_sizes(sections *sec);

// Convergence hardening: did this pass EMIT differently from the previous one? Compares each section's
// {size, crc} against last time's (and notes this pass's for next time). Call on SETTLING passes only:
// the final pass legitimately differs at INCBINs, the output pass at ZA_AUTO addresses.
bool sections_emission_changed(sections *sec);


// ---- copying ----

// A deep copy of one section into the given arena - the copy owns its backing outright (name, code,
// attribute keys and values), so it outlives the per-pass original and the source text itself. This is
// how a caller keeps a result's sections beyond the next assemble.
section section_make_copy(section s, rc_arena *arena);


#endif // ifndef BARON_SECTIONS_H_
