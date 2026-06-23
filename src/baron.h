#ifndef BARON_BARON_H_
#define BARON_BARON_H_

#include "scopes.h"


// Baron's global state, which we thread around by pointer rather than stash in a
// global. For now it is just the scope tree; overlays, output and options will pile
// in later. Set it up in place and then leave it put - its `scopes` holds tries
// that point back into its own pools.
typedef struct baron {
    scopes scopes;
} baron;

void baron_init(baron *b);
void baron_deinit(baron *b);


#endif // ifndef BARON_BARON_H_
