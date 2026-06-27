#ifndef BARON_BARON_H_
#define BARON_BARON_H_

#include "scopes.h"
#include "overlays.h"
#include "source_files.h"


// Baron's global state: the managers that are effectively constant across a parse and that the
// parser threads as a single `baron *` first argument. It holds the scope tree (root already
// made at scope index 0), the overlay manager (default overlay already made at index 0), and the
// source-file cache. Output and options pile in later. Set it up in place and then leave it put -
// its `scopes` holds tries that point back into its own pools.
typedef struct baron {
    scopes       scopes;
    overlays     overlays;
    source_files source_files;
} baron;

void baron_init(baron *b);
void baron_deinit(baron *b);


#endif // ifndef BARON_BARON_H_
