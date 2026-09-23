// libFuzzer harness: any byte string through assemble_string. Built only under BARON_FUZZ
// (clang's -fsanitize=fuzzer supplies main). Baron's contract is that every input either
// assembles or is refused with a diagnostic - a crash, hang or sanitizer report here is a
// finding, never expected behaviour.

#include "assemble.h"
#include "richc/arena.h"
#include "richc/str.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20)) {
        return -1;   // a megabyte of source is plenty; keep iterations brisk
    }

    // Fresh arenas per input, freed whole afterwards: each run is independent, and the
    // sanitizer sees any read past what an assemble legitimately allocated.
    baron_desc desc = {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };

    rc_str text = size > 0 ? rc_str_make((const char *) data, (uint32_t) size) : RC_STR("");
    assemble_string(&desc, RC_STR("fuzz"), text);

    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
    return 0;
}
