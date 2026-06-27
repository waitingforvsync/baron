#ifndef BARON_BARON_H_
#define BARON_BARON_H_

#include "scopes.h"
#include "overlay.h"


// Baron's global state, which we thread around by pointer rather than stash in a
// global. It holds the scope tree (with the root already made at scope index 0), the
// single default overlay, and the arena backing that overlay's object code. More
// overlays, output and options pile in later. Set it up in place and then leave it
// put - its `scopes` holds tries that point back into its own pools.
typedef struct baron {
    scopes   scopes;
    rc_arena code_arena;   // backs the overlay's object code
    overlay  overlay;      // the single default overlay (for now)
} baron;

void baron_init(baron *b);
void baron_deinit(baron *b);


#endif // ifndef BARON_BARON_H_
