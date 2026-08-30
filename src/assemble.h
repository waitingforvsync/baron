#ifndef BARON_ASSEMBLE_H_
#define BARON_ASSEMBLE_H_

#include "richc/bytes.h"
#include "richc/arena.h"
#include "richc/array/str.h"
#include "symbols.h"
#include "scopes.h"
#include "sections.h"
#include "source_files.h"
#include "cursor.h"
#include "error.h"


// The assembler's public face: hand it memory (baron_desc) and a source, get back a read-only snapshot
// (baron_result) of the object code, symbols and diagnostics. The internal machine that does the work -
// the baron struct and its managers (scopes, sections, source files, macros, functions) - is built,
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

// How many PRINT output channels there are: PRINT #n, ... takes a single digit, so exactly ten.
// Channel 0 is the default (no #n), and is also where the -v listing goes.
enum { baron_num_channels = 10 };

// A single diagnostic: a code, where it happened, its severity level, and an optional payload string.
// Diagnostics accumulate into a flat array (in baron) rather than aborting at the first - the location's
// cursor names both the source and the offset, and a further entry can point at a related site (an
// INCLUDE / macro "included from" frame, or the original definition behind a duplicate), so the array
// reads as a stack trace with no per-error metadata. The code lives in error_type, the whole diagnostic
// vocabulary; the renderer substitutes payload for the '%' in the code's message template (a symbol
// name, a branch distance, an ERROR statement's text - {0} when the message stands alone).
typedef struct diagnostic {
    uint16_t   code;       // error_type
    cursor     at;
    uint8_t    severity;   // 0 = error; higher = warning level (see severity_* above)
    rc_str     payload;    // permanent-backed (the recording helper copies), or {0} for none
} diagnostic;

#define RC_ARRAY_TYPE diagnostic
#define RC_ARRAY_NAME diagnostic
#include "richc/template/array.h"

// Everything an assemble runs on: the three arenas (owned by the caller; the internal managers borrow
// them) plus the options. The arenas differ only in lifetime - permanent lives the whole run, per_pass
// resets at the top of every pass, scratch threads by value per call so it self-cleans. Build one
// directly (no constructor; reserves are virtual address space, so defaults are fine), feed it to as
// many assembles as you like, and deinit the three arenas at the end:
//
//     baron_desc desc = {
//         .permanent = rc_arena_make_default(),
//         .per_pass  = rc_arena_make_default(),
//         .scratch   = rc_arena_make_default(),
//     };
//
// A predefine (the CLI's -D) may forward-reference symbols the source defines later; a source
// assignment to the same name is a duplicate - the predefinition stands, so a guarded source default
// wants a DIFFERENT name (IF DEFINED(name) : local = name : ELSE : local = default : ENDIF).
typedef struct baron_desc {
    rc_arena    permanent;   // scopes/symbols, source text, diagnostics, the harvested result
    rc_arena    per_pass;    // sections, macros, functions
    rc_arena    scratch;
    bool        verbose;     // build the assembly listing into channels[0] (one extra pass)
    rc_view_str defines;     // "name=expression" predefines, bound into the root scope each pass
} baron_desc;

// What an assemble produced: a read-only, position-independent snapshot, valid until the next assemble
// on the same arenas supersedes it (sections and channels are per_pass-backed; diagnostics, scopes and
// sources are permanent-backed, so those stay readable until the arenas are freed). passes == 0 means
// failure: sections holds only an empty default, scopes is an empty tree, diagnostics has the errors.
//
// Look a symbol up by full dotted path with baron_result_symbol, or harvest the whole spellable table
// with scopes_view_flatten. A diagnostic's cursor holds a source index + byte offset into sources -
// range-check the index: the unreadable-root diagnostic carries a cursor no source was registered for.
//
// Under -v, channel 0 interleaves PRINT output between the listing lines, and the sections come from
// that same post-allocation pass - so what the listing shows IS the output (without -v the bytes are
// identical, and PRINT writes on the final pass instead).
typedef struct baron_result {
    uint32_t            passes;        // number of passes taken (the listing pass is not counted); 0 == failure
    rc_view_section     sections;      // every object-code section (pc + code); index 0 is the default
    rc_view_diagnostic  diagnostics;   // every error + warning, in order
    rc_view_source_file sources;       // every source touched (name + text), indexed by a cursor's source
    rc_str              channels[baron_num_channels];   // PRINT output per channel; 0 also holds the -v listing
    scopes_view         scopes;        // the resolved scope tree, read-only (query via the functions below)
} baron_result;

// The default section's object code (index 0) - the common single-section case, empty on failure. For
// multiple sections, iterate r->sections directly.
rc_view_bytes baron_result_code(const baron_result *r);

// Look a symbol up in the result by its full dotted path (e.g. "routine.core"), from the top level. Hands
// back value_make_none() if nothing is bound there. A convenience wrapper over scopes_view_get_symbol; for
// the whole table at once, flatten r->scopes with scopes_view_flatten.
value baron_result_symbol(const baron_result *r, rc_str path);

// Assemble a source (a string cached under name, or a file loaded from path) into the default section,
// running passes until labels and forward references settle. Builds a fresh internal machine on arenas,
// runs it, and returns the harvested snapshot; on failure the code/symbols come back empty with the errors
// in diagnostics.
baron_result assemble_string(baron_desc *arenas, rc_str name, rc_str text);
baron_result assemble_file(baron_desc *arenas, rc_str path);


#endif // ifndef BARON_ASSEMBLE_H_
