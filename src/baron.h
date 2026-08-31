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


// The pass's first binding that would not settle (a changed value, or a dead-branch removal):
// changes cascade down the file, so the earliest is nearest the root cause when no_convergence reports.
typedef struct unsettled_note {
    bool   seen;
    cursor at;        // the binding statement
    rc_str payload;   // "'name' (old -> new)", built in per_pass
} unsettled_note;

// Baron's global state, threaded through the parser as a baron * first argument. The current scope
// and section indices thread as parameters instead (they strictly nest), and scratch by value.
// Internal: built fresh inside assemble_string/_file on the caller's borrowed arenas, then discarded.
typedef struct baron {
    rc_arena           *permanent;       // scopes/symbols, source text, diagnostics (never reset)
    rc_arena           *per_pass;        // sections, macros, functions (reset each pass)
    scopes              scopes;
    sections            sections;
    zeropage            zeropage;        // the ZA_POOL set + allocator IR; dormant until ZA_POOL runs
    source_files        source_files;
    macros              macros;          // macro store + its dynamic statement-token table, rebuilt each pass
    functions           functions;       // FUNCTION store + its dynamic operand-token table, rebuilt each pass
    rc_array_diagnostic diagnostics;     // in permanent; empty means the assemble succeeded
    rc_mstr             channels[baron_num_channels];   // PRINT streams (per_pass); channel 0 doubles as the -v listing
    bool                want_verbose;    // run the -v listing pass?
    rc_view_str         defines;         // -D name=expression predefines, bound at the top of each pass
    uint32_t            include_depth;   // the three depth counters guard runaway recursion, reset per pass
    uint32_t            macro_depth;
    uint32_t            function_depth;
    unsettled_note      unsettled;       // for the no_convergence report
} baron;

// Build a fresh baron on the desc's borrowed arenas, seeding the root scope and default section.
// Safe to return by value (every member is position-independent); no deinit - the arenas own everything.
baron baron_make(baron_desc *a);

// Record a diagnostic: baron_error fails the assemble (severity 0), baron_warning (positive level)
// never does. The _payload variants attach the '%'-substituted string, copied into permanent.
void baron_error(baron *b, error_type code, cursor at);
void baron_warning(baron *b, error_type code, cursor at, uint8_t severity);
void baron_error_payload(baron *b, error_type code, cursor at, rc_str payload);
void baron_warning_payload(baron *b, error_type code, cursor at, uint8_t severity, rc_str payload);

// Error-severity (level 0) diagnostics so far; INCLUDE reads the delta to spot a file's own errors.
uint32_t baron_error_count(const baron *b);
bool baron_has_errors(const baron *b);


#endif // ifndef BARON_BARON_H_
