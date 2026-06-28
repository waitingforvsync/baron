#ifndef BARON_BARON_H_
#define BARON_BARON_H_

#include "scopes.h"
#include "overlays.h"
#include "source_files.h"


// Baron's global state, threaded through the parser as a single `baron *` first argument. It holds
// the scope tree (root already made at scope index 0), the overlay manager (default overlay made
// at index 0), the source-file cache, and the current overlay index. The current overlay is flat,
// assembler-wide state - a directive selects it and it stays selected, never scoped by braces or
// files - so it lives here rather than being threaded. Output and options pile in later. Set it
// up in place and leave it put - its `scopes` holds tries that point back into its own pools.
typedef struct baron {
    scopes       scopes;
    overlays     overlays;
    source_files source_files;
    uint32_t     current_overlay;   // the overlay statements emit into now
} baron;

void baron_init(baron *b);
void baron_deinit(baron *b);


#endif // ifndef BARON_BARON_H_
