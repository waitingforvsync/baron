#ifndef BARON_BARON_H_
#define BARON_BARON_H_

#include "scopes.h"
#include "overlays.h"
#include "source_files.h"
#include "macros.h"      // the macro store, and rc_array_token (the dynamic statement table)
#include "functions.h"   // the user-FUNCTION store (and its dynamic operand-token table)
#include "assemble.h"   // error_type, diagnostic, rc_array_diagnostic


// Baron's global state, threaded through the parser as a single `baron *` first argument. It holds
// the scope tree (root already made at scope index 0), the overlay manager (default overlay made
// at index 0), the source-file cache, the accumulated diagnostics, and the current overlay index.
// The current overlay is flat, assembler-wide state - a directive selects it and it stays selected,
// never scoped by braces or files - so it lives here rather than being threaded. Output and options
// pile in later. Set it up in place and leave it put - its `scopes` holds tries that point back into
// its own pools (which is also why an assemble takes a `baron *`: it cannot be returned by value).
typedef struct baron {
    scopes              scopes;
    overlays            overlays;
    source_files        source_files;
    macros              macros;            // the macro store (and its dynamic statement-token table), rebuilt each pass
    functions           functions;         // the user-FUNCTION store (and its dynamic operand-token table), rebuilt each pass
    rc_arena            diag_arena;    // backs the diagnostics array (reset per assemble)
    rc_array_diagnostic diagnostics;   // errors from the last assemble; empty means it succeeded
    uint32_t            current_overlay;   // the overlay statements emit into now
    uint32_t            include_depth;     // how many INCLUDEs deep the parser is now, to catch runaway recursion
    uint32_t            macro_depth;       // how many macro expansions deep, to catch runaway recursion
    uint32_t            function_depth;    // how many FUNCTION calls deep the evaluator is, to catch runaway recursion
} baron;

void baron_init(baron *b);
void baron_deinit(baron *b);

// Append a diagnostic to b's list. baron_error records a failing error (severity 0); baron_warning
// records a harmless warning at a positive `severity` level. baron_has_errors reports whether any
// error-severity diagnostic is present - the pass driver fails the assemble exactly when it is, so
// warnings alone leave it succeeding (and stay in the list for the caller to read / filter by level).
void baron_error(baron *b, error_type code, cursor at);
void baron_warning(baron *b, error_type code, cursor at, uint8_t severity);

// The count of error-severity (level 0) diagnostics recorded so far. baron_has_errors is just this being
// non-zero; INCLUDE also uses it to tell whether an included file added any errors of its own.
uint32_t baron_error_count(const baron *b);
bool baron_has_errors(const baron *b);


#endif // ifndef BARON_BARON_H_
