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


// How loud a diagnostic is. An error fails the assemble; a warning is reported but harmless (a
// dubious-but-legal construct, e.g. an indirect JMP whose vector straddles a page boundary). The
// first enumerator is the zero default, so a bare-recorded diagnostic is an error.
typedef enum diagnostic_severity {
    diagnostic_error,
    diagnostic_warning,
} diagnostic_severity;

// A single diagnostic: a code, where it happened, and how loud it is. Diagnostics accumulate into a
// flat array (in `baron`) rather than aborting at the first - the location's `cursor` names both the
// source and the offset, and a future INCLUDE / macro frame is just a further entry pointing at the
// enclosing site (so the array reads innermost-first as a stack trace, no per-error metadata). The
// code lives in `error_type`, which is really the whole diagnostic vocabulary (errors and
// warnings both); `severity` says which a given occurrence is.
typedef struct diagnostic {
    error_type          code;
    cursor              at;
    diagnostic_severity severity;
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
