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


// One section: a window onto the manager's shared emission stream, with its own pc, a unique name
// and a resolved attribute bag. The window is contiguous because nesting is lexical and emission
// sequential; an explicit org repositions later labels without moving where code lands.
typedef struct section {
    rc_str              name;         // own namespace, separate from symbols and scopes; a view into source text
    uint32_t            pc;           // effective address of the next byte - the value a label takes
    bool                cmos;         // consumed cmos attribute: 65C02 encodings allowed here (never inherited)
    bool                is_guarded;   // consumed guard attribute present? (never inherited)
    uint32_t            guard;        // the guarded 16-bit address: first address emission must not reach (meaningful only when is_guarded)
    uint32_t            begin;        // window into the shared stream: stream.num at creation
    uint32_t            end;          // maintained on every emit here and on every child close
    rc_view_bytes       code;         // the window's bytes, SEALED by sections_seal once the pass stops emitting
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


// The section manager: sections addressed by index, in the same spirit as scopes is for symbols.
// All emission appends to ONE shared stream; every section is a window into it, and ENDSECTION folds
// a child's window into its parent's (the default section at index 0 is the true root, so its window
// ends up covering the whole stream). Sections are per-pass (object code is only read from the final
// pass's output), rebuilt by sections_reset each pass; only the emission fingerprints survive, in the
// permanent arena.
typedef struct sections {
    rc_arena                  *arena;       // borrowed: baron's per_pass arena - stream, nodes, attributes
    rc_arena                  *permanent;   // borrowed: backs emissions, the only cross-pass state
    rc_array_bytes             stream;      // the one shared emission stream, rebuilt each pass
    rc_array_section           nodes;       // index 0 is the default section, the root of the nesting tree
    rc_view_section_emission   emissions;   // per-section {size, crc}, rebuilt in full each settling pass
} sections;

enum { sections_default = 0 };   // index of the default section


// ---- lifecycle ----

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent);

// Rebuild the list for a fresh pass: just the nameless default section at index 0. Call at the top of
// each pass, after the per_pass arena has been reset (this also makes the default the first time round).
void sections_reset(sections *sec);


// ---- queries ----

uint32_t sections_pc(const sections *sec, uint32_t id);

// Section id's bytes so far, computed live from its window. The view is invalidated by ANY section's
// next emission (one shared, growing stream): take .num immediately, or read the bytes before the
// next emit - never hold the view across one.
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


// ---- mutation ----

// Create the named section (an empty window at the stream tail, pc 0 - the caller seeds it from the
// enclosing section - and empty attributes) and return its stable index, or RC_INDEX_NONE if the name
// already exists this pass (names are unique; the caller raises the error). Indices are stable across
// passes because creation order is first-sighting parse order, identical each pass.
uint32_t sections_make(sections *sec, rc_str name);

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

// Append a byte, pc += 1.
void sections_emit_u8(sections *sec, uint32_t id, uint8_t b);

// Append a little-endian word, pc += 2.
void sections_emit_u16(sections *sec, uint32_t id, uint16_t w);

// Append count zero bytes, pc += count.
void sections_skip(sections *sec, uint32_t id, uint32_t count);

// ENDSECTION bookkeeping: fold the closed child into its parent - the parent's window absorbs the
// child's extent and its pc advances by the child's size, so bytes propagate all the way up to the
// default section at index 0.
void sections_close(sections *sec, uint32_t id, uint32_t parent);

// End-of-pass: fill every section's code view as a slice of the stream. Views are only stable once
// the pass stops growing the stream, so they are sealed in one step, never maintained live.
void sections_seal(sections *sec);


// ---- cross-pass bookkeeping ----

// Convergence hardening: did this pass EMIT differently from the previous one? Compares each section's
// {size, crc} against last time's (and notes this pass's for next time). Call on SETTLING passes only:
// the final pass legitimately differs at INCBINs, the output pass at ZA_AUTO addresses.
bool sections_emission_changed(sections *sec);


// ---- copying ----

// A deep copy of one SEALED section into the given arena - the copy owns its backing outright (name,
// code bytes, attribute keys and values), so it outlives the per-pass original, its stream and the
// source text itself. This is how a caller keeps a result's sections beyond the next assemble.
section section_make_copy(section s, rc_arena *arena);


#endif // ifndef BARON_SECTIONS_H_
