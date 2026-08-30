#ifndef BARON_BARON_H_
#define BARON_BARON_H_

#include "scopes.h"
#include "sections.h"
#include "zeropage.h"
#include "source_files.h"
#include "macros.h"
#include "functions.h"
#include "assemble.h"
#include "richc/mstr.h"


// Baron's global state, threaded through the parser as a single baron * first argument: the scope
// tree (root at scope index 0), the section manager, the source-file cache, the macro / function
// stores and the accumulated diagnostics. The current scope and section indices are NOT stored
// here - they thread through the parser as parameters, since scopes and sections strictly nest.
//
// baron is an INTERNAL detail: built on the stack inside assemble_string / assemble_file, BORROWING
// the caller's three arenas (a baron_desc), run, and discarded - the caller sees only the harvested
// baron_result. permanent takes the diagnostics; per_pass is reset at the top of each pass; scratch
// threads by value straight into the pass loop, so it is not stored here. The subsystems below
// borrow permanent (scopes / source_files) or per_pass (sections / macros / functions) in turn.
typedef struct baron {
    rc_arena           *permanent;       // BORROWED from baron_desc: scopes/symbols, source text, diagnostics
    rc_arena           *per_pass;        // BORROWED from baron_desc: sections, macros, functions (reset each pass)
    scopes              scopes;
    sections            sections;
    zeropage            zeropage;        // the ZA_POOL set + allocator IR; dormant until ZA_POOL runs
    source_files        source_files;
    macros              macros;          // the macro store (and its dynamic statement-token table), rebuilt each pass
    functions           functions;       // the user-FUNCTION store (and its dynamic operand-token table), rebuilt each pass
    rc_array_diagnostic diagnostics;     // errors from the last assemble (in permanent); empty means it succeeded
    rc_mstr             channels[baron_num_channels];   // PRINT streams (per_pass), written on the output pass; channel 0 also carries the -v listing
    bool                want_verbose;    // copied from baron_desc.verbose: run the listing pass at all?
    rc_view_str         defines;         // copied from baron_desc.defines: name=expression predefines, bound each pass
    uint32_t            include_depth;   // how many INCLUDEs deep the parser is now, to catch runaway recursion
    uint32_t            macro_depth;     // how many macro expansions deep, to catch runaway recursion
    uint32_t            function_depth;  // how many FUNCTION calls deep the evaluator is, to catch runaway recursion
} baron;

// Build a fresh baron on a's three arenas (borrowing them - they stay the caller's to free) and return it
// by value. Seeds the scope root and the default section, so it is ready to assemble into. There is no
// baron_deinit: the arenas own everything, and the caller frees them (rc_arena_deinit on each).
//
// Returning by value is safe because every member is position-independent: a trie is now a plain { root }
// value (it holds no pointer into its pool - the pool is passed to each op), the managers hold only borrowed
// arena pointers (into a, never into the baron) plus arena-backed arrays, and all inter-container links are
// indices. So the copy a return b; may make - C not guaranteeing copy elision - dangles nothing.
baron baron_make(baron_desc *a);

// Append a diagnostic to b's list. baron_error records a failing error (severity 0); baron_warning
// records a harmless warning at a positive severity level. baron_has_errors reports whether any
// error-severity diagnostic is present - the pass driver fails the assemble exactly when it is, so
// warnings alone leave it succeeding (and stay in the list for the caller to read / filter by level).
// The _payload variants attach a string the renderer substitutes for '%' in the message (a symbol name,
// a branch distance, an ERROR statement's text); it is COPIED into the permanent arena, so any backing
// will do. The plain forms record no payload.
void baron_error(baron *b, error_type code, cursor at);
void baron_warning(baron *b, error_type code, cursor at, uint8_t severity);
void baron_error_payload(baron *b, error_type code, cursor at, rc_str payload);
void baron_warning_payload(baron *b, error_type code, cursor at, uint8_t severity, rc_str payload);

// The count of error-severity (level 0) diagnostics recorded so far. baron_has_errors is just this being
// non-zero; INCLUDE also uses it to tell whether an included file added any errors of its own.
uint32_t baron_error_count(const baron *b);
bool baron_has_errors(const baron *b);


#endif // ifndef BARON_BARON_H_
