#ifndef BARON_SECTIONS_H_
#define BARON_SECTIONS_H_

#include "richc/bytes.h"
#include "value.h"    // value: an attribute's evaluated value
#include "cursor.h"   // cursor: where an attribute was written (for later diagnostics)


// A section attribute: one `key = expr` pair off the SECTION line, resolved to a value at assembly time.
// `key` is a view into the source text (permanent), so it outlives the per-pass section. `v` is copied into
// the section manager's arena (see sections_add_attribute), so its string / list backing is durable too. The
// assembler acts on the keys it owns (`org` sets the address, `cmos` the instruction set); the rest simply
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
    bool                cmos;        // the consumed `cmos` attribute: 65C02 encodings allowed here
    uint32_t            guard;      // the consumed `guard` attribute: the first address emission must
                                    // not reach (RC_INDEX_NONE = unguarded)
    rc_array_bytes      code;
    rc_array_attribute  attributes;
} section;

#define RC_ARRAY_TYPE section
#define RC_ARRAY_NAME section
#include "richc/template/array.h"


// One recorded INCSECTION: `count` zero bytes were reserved at `dst_offset` in section `dst`, to be
// overwritten with the bytes of the section named `src_name` by the post-assembly fixup step. The copy
// cannot happen at parse time - the source may be defined LATER in the source, and the zero-page
// allocator patches operand bytes after the final pass - so copying last, in dependency order, gets both
// right. `at` is the INCSECTION statement, for the unknown-section / circular diagnostics.
typedef struct splice {
    uint32_t dst;          // destination section index
    uint32_t dst_offset;   // where in dst's code buffer the reserved span begins
    rc_str   src_name;     // the named source section (a view into permanent source text)
    uint32_t count;        // bytes reserved this pass - the source's best-known size
    cursor   at;
} splice;

#define RC_ARRAY_TYPE splice
#define RC_ARRAY_NAME splice
#include "richc/template/array.h"

// A section's size as it stood at the end of the last pass - what a splice reserves for a source that is
// not (yet) built this pass. Keyed by name (a view into permanent source text) and kept in the permanent
// arena, because it must SURVIVE the per-pass reset: it is precisely cross-pass memory.
typedef struct section_size {
    rc_str   name;
    uint32_t size;
} section_size;

#define RC_ARRAY_TYPE section_size
#define RC_ARRAY_NAME section_size
#include "richc/template/array.h"


// The section manager: a container of sections addressed by index, in the same spirit as `scopes`
// is for symbols. Index 0 is the default section. Every per-section operation goes through the manager
// and takes an index. Sections are PER-PASS: nothing about them needs to survive a pass (object code
// is only read from the final pass's output, and pc resets every pass), so the list, per-section code,
// attribute bags and the splice records all live in the BORROWED per_pass arena and are rebuilt by
// sections_reset at the top of each pass (after that arena is reset once). The one exception is `sizes`,
// the name -> last-pass-size map splices reserve from: it lives in the borrowed PERMANENT arena and is
// deliberately NOT reset.
// One section's emission fingerprint at the end of a pass: enough to notice the content changing
// between settling passes even when no symbol moved (see sections_emission_changed).
typedef struct section_emission {
    uint32_t size;
    uint32_t crc;
} section_emission;

#define RC_ARRAY_TYPE section_emission
#define RC_ARRAY_NAME section_emission
#include "richc/template/array.h"

typedef struct sections {
    rc_arena                  *arena;       // BORROWED: baron's per_pass arena - nodes, code, attributes, splices
    rc_arena                  *permanent;   // BORROWED: backs `sizes` + `emissions`, the only cross-pass state
    rc_array_section           nodes;       // index 0 is the default section
    rc_array_splice            splices;     // this pass's INCSECTIONs, in statement order
    rc_array_section_size      sizes;       // name -> size at the end of the last pass
    rc_view_section_emission   emissions;   // per-section {size, crc} at the end of the last settling pass
                                            // - rebuilt in full each settling pass, so a view
} sections;

enum { sections_default = 0 };   // index of the default section

void sections_init(sections *sec, rc_arena *per_pass, rc_arena *permanent);

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
void sections_set_cmos(sections *sec, uint32_t id, bool cmos);    // the consumed `cmos` attribute
bool sections_cmos(const sections *sec, uint32_t id);             // are 65C02 encodings allowed here?
void sections_set_guard(sections *sec, uint32_t id, uint32_t addr);   // the consumed `guard` attribute
uint32_t sections_guard(const sections *sec, uint32_t id);        // guard address (RC_INDEX_NONE = unguarded)
void sections_emit_u8(sections *sec, uint32_t id, uint8_t b);     // append a byte, pc += 1
void sections_emit_u16(sections *sec, uint32_t id, uint16_t w);   // little-endian word, pc += 2
void sections_skip(sections *sec, uint32_t id, uint32_t count);   // append `count` zero bytes, pc += count

// Convergence hardening: did this pass EMIT differently from the previous one? Compares every section's
// {length, crc} against the values noted last time (and notes this pass's for next time). The symbol
// table is the only state a pass hands to the next, so a content change with no symbol change cannot
// make the output wrong - but it means something's emission depends on more than the symbols, which is
// worth another pass to let it settle (e.g. an RND draw set shifted by a settling structure). Call it on
// SETTLING passes only: the final pass legitimately differs wherever INCBIN sits (real bytes load only
// there), and the output pass differs wherever a ZA_AUTO address lands. The first pass, with nothing to
// compare against, reports false.
bool sections_emission_changed(sections *sec);

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

// The index of the section named `name`, or RC_INDEX_NONE. A linear scan (sections are few) skipping the
// nameless default.
uint32_t sections_find(const sections *sec, rc_str name);

// Record an INCSECTION: reserve the source's best-known size in `dst` (its live size when it is already
// built this pass, else its size at the end of the last pass, else 0 on a first sighting) as zero bytes -
// pc advances with them - and note the fixup for the post-assembly copy. The end-of-pass settle check
// (sections_splices_changed) demands another pass whenever a reservation missed the real size.
void sections_splice(sections *sec, uint32_t dst, rc_str src_name, cursor at);

// This pass's splice records, in statement order - the assembler's cycle check and final fixup walk them.
rc_view_splice sections_splices(const sections *sec);

// Did any splice reserve a size other than its source's settled size this pass (a forward first sighting,
// or a source that grew/shrank)? Folded into the pass's `changed` so the layout gets another pass. An
// unknown source compares against 0, so a genuinely missing section does not spin passes - the final
// fixup step reports it instead.
bool sections_splices_changed(const sections *sec);

// End-of-pass bookkeeping: record every named section's size into the cross-pass map (and pin a splice
// source that never appeared at 0, so a section that vanished across passes cannot leave a stale size
// spinning the convergence loop).
void sections_note_sizes(sections *sec);

// The fixup copy: overwrite dst's bytes at [offset, offset + src size) with src's whole code buffer. The
// span was reserved by sections_splice and the sizes have settled, so it fits exactly.
void sections_copy_in(sections *sec, uint32_t dst, uint32_t offset, uint32_t src);

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
