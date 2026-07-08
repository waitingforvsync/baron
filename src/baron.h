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
// pile in later. It is built by value with `baron_make`: now that a trie is a position-independent value
// (it holds no pointer into its pool), every member is trivially movable, so the whole struct copies
// cleanly. The parser still threads a `baron *` - to mutate one shared instance, not out of any move
// hazard. `baron` is now an INTERNAL detail: it is built on the stack inside assemble_string / assemble_file,
// BORROWING the caller's three arenas (a `baron_arenas`), run, and discarded - the caller sees only the
// harvested `baron_result`, never this struct. The two borrowed pointers are all it needs: `permanent`
// to push diagnostics into, `per_pass` to reset at the top of each pass. scratch is threaded by value
// straight from the arenas into the pass loop, so it is not stored here. The subsystems below borrow
// `permanent` (scopes / source_files) or `per_pass` (overlays / macros / functions) in turn.
typedef struct baron {
    rc_arena           *permanent;   // BORROWED from baron_arenas: scopes/symbols, source text, diagnostics
    rc_arena           *per_pass;    // BORROWED from baron_arenas: overlays, macros, functions (reset each pass)
    scopes              scopes;
    overlays            overlays;
    source_files        source_files;
    macros              macros;            // the macro store (and its dynamic statement-token table), rebuilt each pass
    functions           functions;         // the user-FUNCTION store (and its dynamic operand-token table), rebuilt each pass
    rc_array_diagnostic diagnostics;   // errors from the last assemble (in permanent); empty means it succeeded
    uint32_t            current_overlay;   // the overlay statements emit into now
    uint32_t            include_depth;     // how many INCLUDEs deep the parser is now, to catch runaway recursion
    uint32_t            macro_depth;       // how many macro expansions deep, to catch runaway recursion
    uint32_t            function_depth;    // how many FUNCTION calls deep the evaluator is, to catch runaway recursion
} baron;

// Build a fresh baron on `a`'s three arenas (borrowing them - they stay the caller's to free) and return it
// by value. Seeds the scope root and the default overlay, so it is ready to assemble into. There is no
// baron_deinit: the arenas own everything, and baron_arenas_deinit frees them.
//
// Returning by value is safe because every member is position-independent: a trie is now a plain `{ root }`
// value (it holds no pointer into its pool - the pool is passed to each op), the managers hold only borrowed
// arena pointers (into `a`, never into the baron) plus arena-backed arrays, and all inter-container links are
// indices. So the copy a `return b;` may make - C not guaranteeing copy elision - dangles nothing.
baron baron_make(baron_arenas *a);

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
