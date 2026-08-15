#ifndef BARON_ASSEMBLE_H_
#define BARON_ASSEMBLE_H_

#include "richc/bytes.h"   // rc_view_bytes, rc_str
#include "richc/arena.h"   // rc_arena
#include "symbols.h"       // symbol_entry, rc_view_symbol_entry (the harvest's output; brings value.h)
#include "scopes.h"        // scopes_view: the read-only scope tree a result carries
#include "sections.h"      // section, rc_view_section: the object-code sections a result carries
#include "source_files.h"  // source_file, rc_view_source_file: the sources a result's cursors index into
#include "cursor.h"        // cursor: where a diagnostic points
#include "error.h"         // error_type (the diagnostic code)


// The assembler's public face: hand it memory (baron_desc) and a source, get back a read-only snapshot
// (baron_result) of the object code, symbols and diagnostics. The internal machine that does the work -
// the `baron` struct and its managers (scopes, sections, source files, macros, functions) - is built,
// run, and discarded inside the entry points below; a caller never names it.


// A diagnostic's severity is a LEVEL, not a category. 0 is an error: always reported, and the only thing
// that fails the assemble. A positive level is a warning - never fatal, and reported only when the
// configured warning level is at least that high. Level 1 warnings are on by default; level 2+ are opt-in
// (optimisation hints and the like - things you want when tuning, not ordinarily). Held as a small int so
// a finer scale later needs no new type. (The command line will set the display threshold; default 1.)
enum {
    severity_error    = 0,
    severity_warning  = 1,   // reported by default
    severity_optional = 2,   // reported only when the warning level is raised
};

// How many PRINT output channels there are: `PRINT #n, ...` takes a single digit, so exactly ten.
// Channel 0 is the default (no #n), and is also where the -v listing goes.
enum { baron_num_channels = 10 };

// A single diagnostic: a code, where it happened, and its severity level. Diagnostics accumulate into a
// flat array (in `baron`) rather than aborting at the first - the location's `cursor` names both the
// source and the offset, and a further entry can point at a related site (an INCLUDE / macro "included
// from" frame, or the original definition behind a duplicate), so the array reads as a stack trace with
// no per-error metadata. The code lives in `error_type`, the whole diagnostic vocabulary.
typedef struct diagnostic {
    error_type code;
    cursor     at;
    uint8_t    severity;   // 0 = error; higher = warning level (see severity_* above)
} diagnostic;

#define RC_ARRAY_TYPE diagnostic
#define RC_ARRAY_NAME diagnostic
#include "richc/template/array.h"

// Everything an assemble runs on: the three arenas (owned by the caller, passed by pointer so the internal
// managers can borrow them) plus the options. The arenas differ only in lifetime: `permanent` lives the
// whole run (scopes/symbols, source text, diagnostics, and the harvested result), `per_pass` is reset at
// the top of every pass (sections, macros, functions), and `scratch` is threaded by value per call so it
// self-cleans. The caller builds one directly - there is no constructor to learn:
//
//     baron_desc desc = {
//         .permanent = rc_arena_make_default(),
//         .per_pass  = rc_arena_make_default(),
//         .scratch   = rc_arena_make_default(),
//     };
//
// (reserves are virtual address space, so defaults are fine for all three), sets any options, feeds it to
// as many assembles as it likes, and deinits the three arenas at the end. Each assemble is independent -
// it does not reset the arenas between runs, so a result stays valid until the NEXT assemble on the same
// desc (which supersedes it).
// `verbose` asks for the assembly listing (baron_result.channels[0]): it costs one extra pass over the
// source, so it is off unless someone wants it.
typedef struct baron_desc {
    rc_arena permanent;
    rc_arena per_pass;
    rc_arena scratch;
    bool     verbose;   // build the assembly listing (one extra pass; see baron_result.channels)
} baron_desc;

// What an assemble produced: a read-only, position-independent snapshot. Every field borrows from the arenas
// that were passed in, so the result is valid until the next assemble on those same arenas (or until
// the arenas are freed). passes == 0 means failure - `sections` then holds only an empty default section,
// `scopes` is an empty tree, and `diagnostics` carries the errors. `sections` is the whole object-code
// list (index 0 is the default section; each carries its pc + code) - iterate it directly, or use
// baron_result_code for the default section's bytes alone. `scopes` is a read-only view of the resolved
// scope tree (its backing outlives the internal machine): look a single symbol up by full dotted path with
// baron_result_symbol / scopes_view_get_symbol, or harvest the whole spellable table on demand with
// scopes_view_flatten (which needs an arena to build the paths into). `sources` is every source the assemble
// touched (the root plus anything INCLUDEd), each a name + full text: a diagnostic's cursor holds a source
// INDEX and a byte OFFSET, and this view is what turns them back into a file name and a line/column. Range-
// check the index before resolving - the one diagnostic with nowhere real to point (an unreadable root file)
// carries a cursor no source was ever registered for.
// Lifetimes differ by field: `sections` borrow from the per_pass arena and are superseded by the next
// assemble on the same arenas; `diagnostics`, `scopes` and `sources` are permanent-backed, so earlier
// results' copies of those remain readable (if superseded) until the desc's arenas are freed.
// `channels` is the PRINT output, one text stream per channel (`PRINT #n, ...`; no #n means 0). Channel 0
// also carries the assembly listing when the desc asked for it (baron_desc.verbose): the listing pass then
// runs after zero-page allocation and both write the same buffer, so PRINT output lands interleaved between
// listing lines - and the returned `sections` come from that pass too, so what the listing shows IS the
// output (without -v, the sections are the settling pass's, patched by the allocator - the same bytes either
// way, and PRINT writes on that pass instead). Same per_pass lifetime as `sections`; all empty when the
// assemble failed.
typedef struct baron_result {
    uint32_t            passes;        // number of passes taken (the listing pass is not counted); 0 == failure
    rc_view_section     sections;      // every object-code section (pc + code); index 0 is the default
    rc_view_diagnostic  diagnostics;   // every error + warning, in order
    rc_view_source_file sources;       // every source touched (name + text), indexed by a cursor's source
    rc_str              channels[baron_num_channels];   // PRINT output per channel; 0 also holds the -v listing
    scopes_view         scopes;        // the resolved scope tree, read-only (query via the functions below)
} baron_result;

// The default section's object code (index 0) - the common single-section case, empty on failure. For
// multiple sections, iterate `r->sections` directly.
rc_view_bytes baron_result_code(const baron_result *r);

// Look a symbol up in the result by its full dotted path (e.g. "routine.core"), from the top level. Hands
// back value_make_none() if nothing is bound there. A convenience wrapper over scopes_view_get_symbol; for
// the whole table at once, flatten `r->scopes` with scopes_view_flatten.
value baron_result_symbol(const baron_result *r, rc_str path);

// Assemble a source (a string cached under `name`, or a file loaded from `path`) into the default section,
// running passes until labels and forward references settle. Builds a fresh internal machine on `arenas`,
// runs it, and returns the harvested snapshot; on failure the code/symbols come back empty with the errors
// in `diagnostics`.
baron_result assemble_string(baron_desc *arenas, rc_str name, rc_str text);
baron_result assemble_file(baron_desc *arenas, rc_str path);



#endif // ifndef BARON_ASSEMBLE_H_
