#ifndef BARON_SECTIONS_H_
#define BARON_SECTIONS_H_

#include "richc/bytes.h"
#include "value.h"    // value: an attribute's evaluated value
#include "cursor.h"   // cursor: where an attribute was written (for later diagnostics)


// A section attribute: one `key = expr` pair off the SECTION line, resolved to a value at assembly time.
// `key` is a view into the source text (permanent), so it outlives the per-pass section. `v` is copied into
// the section manager's arena (see sections_add_attribute), so its string / list backing is durable too. The
// assembler acts on the keys it owns (`org` sets the address, `cpu` the instruction set); the rest simply
// ride here for the output utility to read out of the result. `at` points the diagnostic at this pair.
typedef struct attribute {
    rc_str key;
    value  v;
    cursor at;
} attribute;

#define RC_ARRAY_TYPE attribute
#define RC_ARRAY_NAME attribute
#include "richc/template/array.h"


// One section: a contiguous block of object code with its own instruction pointer, a UNIQUE name, and its
// resolved attribute bag. pc is the effective address of the next byte - the value a label takes. The byte
// array fills from index 0 independent of pc, so an explicit `org` repositions later labels without moving
// where code lands. `name` is the section's own identity in a namespace SEPARATE from symbols and scopes
// (the default section at index 0 is nameless, {0}); a SECTION block selects it. The name is a view into the
// source text (permanent), so it outlives the per-pass section it labels.
typedef struct section {
    rc_str              name;
    uint32_t            pc;
    rc_array_bytes      code;
    rc_array_attribute  attributes;
} section;

#define RC_ARRAY_TYPE section
#define RC_ARRAY_NAME section
#include "richc/template/array.h"


// The section manager: a container of sections addressed by index, in the same spirit as `scopes`
// is for symbols. Index 0 is the default section. Every per-section operation goes through the manager
// and takes an index. Sections are PER-PASS: nothing about them needs to survive a pass (object code
// is only read from the final pass's output, and pc resets every pass), so the whole manager - list,
// per-section code and attribute bags alike - lives in the BORROWED per_pass arena and is rebuilt by
// sections_reset at the top of each pass (after that arena is reset once).
typedef struct sections {
    rc_arena        *arena;   // BORROWED: baron's per_pass arena, backs the nodes AND every section's code
    rc_array_section nodes;   // index 0 is the default section
} sections;

enum { sections_default = 0 };   // index of the default section

void sections_init(sections *sec, rc_arena *per_pass);

// Per-section queries.
uint32_t      sections_pc(const sections *sec, uint32_t id);
rc_view_bytes sections_code(const sections *sec, uint32_t id);

// The whole section list as a read-only view (index 0 is the default). This is what a baron_result hands
// back so a caller can see every section's pc + code + attributes, not just the default one's bytes.
rc_view_section sections_all(const sections *sec);

// The resolved attribute bag of section `id`, for the result / output side (empty if it has none).
rc_view_attribute sections_attributes(const sections *sec, uint32_t id);

// Per-section mutation (manager + index, as per scopes).
void sections_org(sections *sec, uint32_t id, uint32_t addr);     // set pc; does not move code
void sections_emit_u8(sections *sec, uint32_t id, uint8_t b);     // append a byte, pc += 1
void sections_emit_u16(sections *sec, uint32_t id, uint16_t w);   // little-endian word, pc += 2
void sections_skip(sections *sec, uint32_t id, uint32_t count);   // append `count` zero bytes, pc += count

// Post-hoc patch: add `delta` (mod 256) to the byte already emitted at `offset` in section `id`. Used by the
// zero-page allocator to fold a variable's assigned base address into an operand that was emitted with the
// placeholder base (so the emitted byte held just the intra-variable offset). Does NOT touch pc.
void sections_patch_add_u8(sections *sec, uint32_t id, uint32_t offset, uint8_t delta);

// Create the section named `name` (its own pc 0, empty code buffer, empty attribute bag) and return its
// stable index. Section names are UNIQUE: if one already exists this pass, this makes nothing and returns
// RC_INDEX_NONE so the caller can raise error_type_duplicate_section. `name` must be non-empty (the nameless
// default is index 0). The index is stable across passes because sections are created in first-sighting parse
// order, identical each pass.
uint32_t sections_make(sections *sec, rc_str name);

// Add (or, for an already-present key, replace) one attribute on section `id`. `v` is copied into the manager
// arena so its backing survives the pass; `key` is kept as-is (a view into permanent source text). Upsert
// semantics carry the inherit-then-override rule cleanly: a nested section copies its parent's bag first, then
// its own keys replace in place.
void sections_add_attribute(sections *sec, uint32_t id, rc_str key, value v, cursor at);

// Rebuild the list for a fresh pass: the default section at index 0 (nameless, pc 0, an empty code buffer).
// Call it at the top of each pass, after the per_pass arena has been reset. It also makes the default the
// first time round (there is no separate sections_make_default).
void sections_reset(sections *sec);

// A deep copy of one section into the given arena - the copy owns its backing outright (name, code bytes,
// attribute keys and values alike), so it outlives the per-pass original AND the source text its name was a
// view into. This is how a caller keeps a result's sections beyond the next assemble: copy the ones worth
// keeping before they are superseded (mirrors value_make_copy's promotion role).
section section_make_copy(section s, rc_arena *arena);


#endif // ifndef BARON_SECTIONS_H_
