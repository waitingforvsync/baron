#ifndef BARON_OUTPUT_H_
#define BARON_OUTPUT_H_

#include "sections.h"   // section, rc_view_section: what an output spec is built from


// The output stage: turning a run's saved sections into things on the host filesystem. An output_spec is
// the seam between its two halves - the CLI (today; an output script, one day) PRODUCES one from the
// sections plus its switches, and the writers (loose files below, disc images in disc_ssd.h) CONSUME it.
// A writer only ever sees resolved entries - bytes, addresses, a filename - so a new target format is
// simply a new consumer of this one type.

// One file-shaped output: a section that asked to be saved, with its attributes resolved. `filename` is
// the section's `filename` attribute, used verbatim as the host name; a leading "X." pair is read as a
// DFS directory specifier by the writers that care. load/exec stay full 32-bit values here - the DFS
// writer truncates them to the 18 bits its catalogue holds.
typedef struct output_entry {
    rc_str        filename;
    uint32_t      load;
    uint32_t      exec;
    rc_view_bytes code;
} output_entry;

#define RC_ARRAY_TYPE output_entry
#define RC_ARRAY_NAME output_entry
#include "richc/template/array.h"

// Everything a writer needs: the disc-level metadata (straight off the command line; the loose-files
// writer ignores it) and the entries in the order they should land on a disc.
typedef struct output_spec {
    rc_str                title;   // disc title, up to 12 characters
    uint32_t              boot;    // *OPT 4 boot option, 0-3
    uint32_t              cycle;   // catalogue cycle count, 0-99
    rc_view_output_entry  entries;
} output_spec;

// output_spec_make's return: the spec, or a human-readable complaint (empty = success). Errors here are
// configuration mistakes (a `filename` attribute that is not a string, say), reported against the
// section's name rather than a source location - the assemble itself already succeeded.
typedef struct output_spec_result {
    output_spec spec;
    rc_str      error;
} output_spec_result;

// Build a spec from the saved sections: every section carrying a non-empty `filename` attribute becomes
// an entry, in section order - naming the file IS the request to save (an empty filename cancels an
// inherited one). load comes from the `load` attribute, else `org` (failing that, pc minus the code
// length - where the code started); exec from the `exec` attribute, else load.
output_spec_result output_spec_make(rc_view_section sections, rc_str title, uint32_t boot, uint32_t cycle,
                                    rc_arena *arena);

// The loose-files writer: each entry's bytes to `filename` inside the directory `dir` (empty = the
// current one; the directory must already exist). `inf` also writes a "<name>.inf" sidecar beside each
// ("$.NAME <load> <exec> <length>" - the BBC-world interchange form that carries the addresses a bare
// host file loses). Returns a complaint, empty on success.
rc_str output_write_files(const output_spec *spec, rc_str dir, bool inf, rc_arena *arena);


#endif // ifndef BARON_OUTPUT_H_
