#ifndef BARON_ASSEMBLE_H_
#define BARON_ASSEMBLE_H_

#include "richc/bytes.h"   // rc_view_bytes, rc_str
#include "richc/arena.h"   // rc_arena
#include "symbols.h"       // symbol_entry, rc_view_symbol_entry (the harvest's output; brings value.h)
#include "scopes.h"        // scopes_view: the read-only scope tree a result carries
#include "overlays.h"      // overlay, rc_view_overlay: the object-code overlays a result carries
#include "cursor.h"        // cursor: where a diagnostic points
#include "error.h"         // error_type (the diagnostic code)


// The assembler's public face: hand it memory (baron_arenas) and a source, get back a read-only snapshot
// (baron_result) of the object code, symbols and diagnostics. The internal machine that does the work -
// the `baron` struct and its managers (scopes, overlays, source files, macros, functions) - is built,
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

// The three arenas an assemble runs on, owned by the caller and passed in by pointer so the internal
// managers can borrow them. They differ only in lifetime: `permanent` lives the whole run (scopes/symbols,
// source text, diagnostics, and the harvested result), `per_pass` is reset at the top of every pass
// (overlays, macros, functions), and `scratch` is threaded by value per call so it self-cleans. Make them
// once with baron_arenas_make, feed them to as many assembles as you like, and free them with
// baron_arenas_deinit at the end. Each assemble is independent - it does not reset these between runs, so a
// result stays valid until the NEXT assemble on the same arenas (which supersedes it).
typedef struct baron_arenas {
    rc_arena permanent;
    rc_arena per_pass;
    rc_arena scratch;
} baron_arenas;

baron_arenas baron_arenas_make(void);        // permanent + scratch default reserves; per_pass 64 MB
void         baron_arenas_deinit(baron_arenas *a);

// What an assemble produced: a read-only, position-independent snapshot. Every field borrows from the arenas
// that were passed in, so the result is valid until the next assemble on those same arenas (or until
// baron_arenas_deinit). passes == 0 means failure - `overlays` then holds only an empty default overlay,
// `scopes` is an empty tree, and `diagnostics` carries the errors. `overlays` is the whole object-code
// list (index 0 is the default overlay; each carries its pc + code) - iterate it directly, or use
// baron_result_code for the default overlay's bytes alone. `scopes` is a read-only view of the resolved
// scope tree (its backing outlives the internal machine): look a single symbol up by full dotted path with
// baron_result_symbol / scopes_view_get_symbol, or harvest the whole spellable table on demand with
// scopes_view_flatten (which needs an arena to build the paths into).
typedef struct baron_result {
    uint32_t           passes;        // number of passes taken; 0 == failure
    rc_view_overlay    overlays;      // every object-code overlay (pc + code); index 0 is the default
    rc_view_diagnostic diagnostics;   // every error + warning, in order
    scopes_view        scopes;        // the resolved scope tree, read-only (query via the functions below)
} baron_result;

// The default overlay's object code (index 0) - the common single-overlay case, empty on failure. For
// multiple overlays, iterate `r->overlays` directly.
rc_view_bytes baron_result_code(const baron_result *r);

// Look a symbol up in the result by its full dotted path (e.g. "routine.core"), from the top level. Hands
// back value_make_none() if nothing is bound there. A convenience wrapper over scopes_view_get_symbol; for
// the whole table at once, flatten `r->scopes` with scopes_view_flatten.
value baron_result_symbol(const baron_result *r, rc_str path);

// Assemble a source (a string cached under `name`, or a file loaded from `path`) into the default overlay,
// running passes until labels and forward references settle. Builds a fresh internal machine on `arenas`,
// runs it, and returns the harvested snapshot; on failure the code/symbols come back empty with the errors
// in `diagnostics`.
baron_result assemble_string(baron_arenas *arenas, rc_str name, rc_str text);
baron_result assemble_file(baron_arenas *arenas, rc_str path);



#endif // ifndef BARON_ASSEMBLE_H_
