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


// One section: its own bytes, its own pc, a unique name and a resolved attribute bag. ENDSECTION copies
// a nested section's bytes onto the end of its parent's; a top-level section stands alone (the default
// section only ever holds what was written outside every section). A section whose close left its
// parent untouched may be written again: a reopened piece carries on where the last one stopped.
typedef struct section {
    rc_str              name;         // own namespace, separate from symbols and scopes; a view into source text
    uint32_t            parent;       // the section it was first written inside; RC_INDEX_NONE for the default
    uint32_t            pc;           // effective address of the next byte - the value a label takes
    uint32_t            size;         // bytes placed here, children included: code.num, or a virtual section's extent
    bool                cmos;         // consumed cmos attribute: 65C02 encodings allowed here (never inherited)
    bool                is_virtual;   // consumed virtual attribute: assigns addresses, keeps no bytes
    bool                is_guarded;   // consumed guard attribute present? (never inherited)
    bool                is_open;      // between its SECTION and ENDSECTION right now
    bool                reopenable;   // its close left the parent untouched, so it may be written again
    bool                overrun;      // pc just went past &10000; cleared by sections_take_overrun
    uint32_t            guard;        // the guarded 16-bit address: first address emission must not reach (meaningful only when is_guarded)
    rc_array_bytes      code;         // the bytes it keeps (always empty for a virtual section)
    rc_array_attribute  attributes;
} section;

#define RC_ARRAY_TYPE section
#define RC_ARRAY_NAME section
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


// What a transformed section's transform last made of its bytes: the input it was given and the output
// it produced, kept (in permanent) across passes and rounds. The same input never runs the FUNCTION
// twice, and the output's size is the room the section takes in its parent until the transform runs
// again - during settling passes, which never run it.
typedef struct section_transform {
    rc_str        name;     // the section's name
    rc_view_bytes input;
    rc_view_bytes output;
} section_transform;

#define RC_ARRAY_TYPE section_transform
#define RC_ARRAY_NAME section_transform
#include "richc/template/array.h"

// A cached transform output: found only if the transform has run on exactly these input bytes.
typedef struct section_transform_lookup {
    bool          found;
    rc_view_bytes output;
} section_transform_lookup;

// The section manager: sections addressed by index, in the same spirit as scopes is for symbols.
// Emission appends to the innermost open section's own bytes. Sections are per-pass (object code is only
// read from the final pass's output), rebuilt by sections_reset each pass; only the emission fingerprints
// survive, in the permanent arena - and they double as next pass's capacity hints, so after the first pass
// each section's buffer is allocated once at its full size and never moves.
typedef struct sections {
    rc_arena                  *arena;       // borrowed: baron's per_pass arena - nodes, code, attributes
    rc_arena                  *permanent;   // borrowed: backs emissions, the only cross-pass state
    rc_array_section           nodes;       // index 0 is the default section
    rc_view_section_emission   emissions;   // per-section {size, crc}, rebuilt in full each settling pass
    rc_array_section_transform transforms;  // in permanent: each transformed section's last input and output
} sections;

enum { sections_default = 0 };   // index of the default section
enum { sections_address_space = 0x10000 };   // the 6502's 64 KB: a pc may reach &10000, but no byte may land there


// ---- lifecycle ----

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent);

// Rebuild the list for a fresh pass: just the nameless default section at index 0. Call at the top of
// each pass, after the per_pass arena has been reset (this also makes the default the first time round).
void sections_reset(sections *sec);


// ---- queries ----

uint32_t sections_pc(const sections *sec, uint32_t id);

// Section id's bytes so far. The view is invalidated by the next emission into that section, or a child
// closing into it (either may move the buffer): take .num immediately, or read the bytes first.
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

// Does section id carry a guard (the consumed guard attribute)?
bool sections_is_guarded(const sections *sec, uint32_t id);

// Section id's guarded 16-bit address. Only a guarded section has one - ask sections_is_guarded first.
uint32_t sections_guard(const sections *sec, uint32_t id);

// Is section id virtual (the consumed virtual attribute) - assigning addresses but keeping no bytes?
bool sections_is_virtual(const sections *sec, uint32_t id);

// Did section id's SECTION line give it an org of its own? Read off the attribute bag, so a
// forward-referenced org counts from the first pass: the answer decides how the section closes.
bool sections_has_org(const sections *sec, uint32_t id);

// Is section id between its SECTION and ENDSECTION right now?
bool sections_is_open(const sections *sec, uint32_t id);

// May section id be written again - closed, and its close left the parent untouched?
bool sections_can_reopen(const sections *sec, uint32_t id);


// ---- mutation ----

// Create and open the named section inside parent (no bytes, continuing the parent's pc until an org
// rephases it, and empty attributes) and return its stable index, or RC_INDEX_NONE if the name already
// exists this pass (names are unique; the caller raises the error, or reopens it). Indices are stable
// across passes because creation order is first-sighting parse order, identical each pass.
uint32_t sections_make(sections *sec, rc_str name, uint32_t parent);

// Add (or, for an already-present key, replace) one attribute on section id: a key repeated on one
// SECTION line, last wins.
void sections_add_attribute(sections *sec, uint32_t id, rc_str key, value v, cursor at);

// Set section id's pc; does not move code already emitted. BBC host addresses are 32-bit
// (&FFFFxxxx = the I/O processor), so addr may carry the full value - the pc keeps the low 16 bits,
// the 6502's actual address; the attribute bag keeps the whole thing for the output stage.
void sections_org(sections *sec, uint32_t id, uint32_t addr);

// Set the consumed cmos attribute: allow 65C02 encodings in section id.
void sections_set_cmos(sections *sec, uint32_t id, bool cmos);

// Set the consumed guard attribute: the first address emission in section id must not reach. Like
// sections_org, addr may be a full 32-bit host address; the guard keeps the low 16 bits.
void sections_set_guard(sections *sec, uint32_t id, uint32_t addr);

// Set the consumed virtual attribute.
void sections_set_virtual(sections *sec, uint32_t id, bool is_virtual);

// Append a byte, pc += 1 (a virtual section only advances the pc).
void sections_emit_u8(sections *sec, uint32_t id, uint8_t b);

// Append a little-endian word, pc += 2.
void sections_emit_u16(sections *sec, uint32_t id, uint16_t w);

// Append count zero bytes, pc += count (a virtual section only advances the pc).
void sections_skip(sections *sec, uint32_t id, uint32_t count);

// Overwrite a byte section id has already emitted, offset bytes into its code: for a length that is
// only known once everything it counts has gone out (a spliced BASIC line's). A virtual section kept no
// bytes to overwrite, so the caller asks sections_is_virtual first.
void sections_set_u8(sections *sec, uint32_t id, uint32_t offset, uint8_t b);

// Did section id's pc go past the top of memory since we last asked? True at most once a pass, since a
// pc never comes back down. Our own emission counts, and so does a child with its own org closing into
// us; a child continuing our addresses does not - it reported the same crossing itself.
bool sections_take_overrun(sections *sec, uint32_t id);

// Reopen a closed section that sections_can_reopen allows: its next piece carries on where it stopped.
void sections_reopen(sections *sec, uint32_t id);

// Replace section id's bytes (and size) with code - what its transform produced, or the zeroes standing
// in for that during a settling pass. Its pc, and so its labels, are left alone: they are where the
// untransformed bytes will run.
void sections_set_code(sections *sec, uint32_t id, rc_view_bytes code);

// ENDSECTION bookkeeping: close section id and settle it with the parent it was first written inside.
//   - parent is the default: nothing (a top-level section stands alone)
//   - real child, real parent: the child's bytes are copied onto the parent's, whose pc moves past them
//   - virtual child, real parent: nothing (a real section's pc only moves with its bytes)
//   - virtual parent: its pc moves past a child that continued its addresses; a child with its own org
//     is placed elsewhere and takes no room
// Whichever case left the parent untouched makes the section reopenable.
void sections_close(sections *sec, uint32_t id);


// ---- cross-pass bookkeeping ----

// Convergence hardening: did this pass EMIT differently from the previous one? Compares each section's
// {size, crc} against last time's (and notes this pass's for next time). Call on SETTLING passes only:
// the final pass legitimately differs at INCBINs, the output pass at ZA_AUTO addresses.
bool sections_emission_changed(sections *sec);


// The room transformed section name takes in its parent until its transform runs: the size of what it
// last produced, or 0 if it has never run.
uint32_t sections_transform_reserved(const sections *sec, rc_str name);

// What section name's transform produced from exactly these input bytes, if it has run on them before.
section_transform_lookup sections_transform_lookup(const sections *sec, rc_str name, rc_view_bytes input);

// Record what section name's transform made of input (both copied into permanent). True when the output's
// size differs from the room reserved for it: everything laid out after it was laid out wrongly.
bool sections_transform_note(sections *sec, rc_str name, rc_view_bytes input, rc_view_bytes output);

// ---- copying ----

// A deep copy of one section into the given arena - the copy owns its backing outright (name, code
// bytes, attribute keys and values), so it outlives the per-pass original and the source text itself. This is how a caller keeps a result's sections beyond the next assemble.
section section_make_copy(section s, rc_arena *arena);


#endif // ifndef BARON_SECTIONS_H_
