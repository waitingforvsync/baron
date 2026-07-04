#ifndef BARON_ASSEMBLE_H_
#define BARON_ASSEMBLE_H_

#include "richc/bytes.h"   // rc_view_bytes, rc_str
#include "richc/arena.h"   // rc_arena (taken by value)
#include "cursor.h"        // cursor: where a diagnostic points
#include "error.h"         // error_type (the diagnostic code)


// The assembler: parse 6502 source, lay down object code in the default overlay, define labels
// and symbols, and run passes until everything settles. The managers it works through (scopes,
// overlays, source files) live in `baron`, which the entry points need only by pointer.
typedef struct baron baron;


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

// Assemble a source into b's default overlay, running passes until labels and forward references
// settle (or reporting non-convergence). Each entry point caches its source as a source file in
// b (the text under a name, or a loaded file) and then runs the shared pass loop from that source
// index. scratch is a working arena taken by value, so the caller's copy is left untouched.
//
// Returns the number of passes it took, or 0 on failure. Everything of interest is then read back
// from b: object code via overlays_code(&b->overlays, ...), symbols via scopes_get_symbol, and any
// errors from b->diagnostics. On failure the object code and scopes are cleared (only the
// diagnostics remain), so partial or erroneous output is never consumed.
uint32_t assemble_string(baron *b, rc_str name, rc_str text, rc_arena scratch);
uint32_t assemble_file(baron *b, rc_str path, rc_arena scratch);



#endif // ifndef BARON_ASSEMBLE_H_
