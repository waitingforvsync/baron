#ifndef BARON_DISC_SSD_H_
#define BARON_DISC_SSD_H_

#include "output.h"   // output_spec: what a disc image is built from


// The .ssd writer: an output_spec rendered as a single-sided Acorn DFS disc image. Pure byte work into a
// caller arena - nothing here touches the filesystem, so the catalogue is testable byte for byte and the
// CLI decides where the image actually goes.

// The image, or a human-readable complaint (empty = success; the image is only valid then). The disc is
// an 80-track one (the catalogue declares 800 sectors), but the file is truncated after the last used
// sector - emulators happily treat the missing remainder as zeroes, and it keeps a mostly-empty disc
// small on the host.
typedef struct disc_ssd_result {
    rc_view_bytes image;   // built in full by disc_ssd_make
    rc_str         error;
} disc_ssd_result;

disc_ssd_result disc_ssd_make(const output_spec *spec, rc_arena *arena);


#endif // ifndef BARON_DISC_SSD_H_
