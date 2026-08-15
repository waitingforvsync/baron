#include "assemble.h"
#include "report.h"

#include <stdio.h>
#include <string.h>

#ifdef BARON_TESTS
#include "richc/test.h"
#endif


// baron <list of source files>. Each file is assembled in a fresh environment - symbols never leak between
// files (the job BeebAsm's CLEAR used to do) - and its diagnostics are reported as soon as it finishes.
// Success is silent; any error anywhere makes the exit code 1 (but every file is still assembled first, so
// one run reports everything). The sections of each successful file are deep-copied into `saved`: nothing
// reads them yet, but they are the deliverable the coming output stage (-t) will write from, kept alive
// here precisely because the next assemble_file supersedes the previous result's sections.
int main(int argc, char **argv)
{
#ifdef BARON_TESTS
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        return rc_test_run(argc > 2 ? argv[2] : "");
    }
#endif

    // Options first: -v prints each file's assembly listing. Anything else dash-shaped is refused (so -t
    // and friends stay free to claim later), and at least one real file must remain.
    bool verbose = false;
    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "baron: unknown option '%s'\nusage: baron [-v] <source files>\n", argv[i]);
            return 1;
        }
        else {
            files++;
        }
    }
    if (files == 0) {
        fprintf(stderr, "usage: baron [-v] <source files>\n");
        return 1;
    }

    // One arena for everything the CLI itself keeps (section copies, rendered reports); one baron_desc -
    // arenas plus options - REUSED across all files. Reuse is safe because nothing outlives its turn: each
    // report is printed before the next assemble supersedes the result it came from, and the sections worth
    // keeping are copied.
    rc_arena cli = rc_arena_make_default();
    baron_desc desc = {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
        .verbose   = verbose,   // -v: ask the assembler for the listing pass
    };
    rc_array_section saved = rc_array_section_make(8, &cli);
    bool failed = false;
    bool listed_any = false;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            continue;   // options were handled above
        }
        rc_str path = rc_str_from_cstr(argv[i]);
        baron_result r = assemble_file(&desc, path);

        rc_str rep = report_render(&r, path, severity_warning, &cli);
        if (rep.len != 0) {
            fprintf(stderr, "%.*s", (int) rep.len, rep.data);
        }

        if (r.passes != 0) {
            // The listing goes to stdout (it is the product; diagnostics are commentary), one blank line
            // between files so a multi-file run reads as chapters.
            if (verbose && r.verbose.len != 0) {
                fprintf(stdout, "%s%.*s", listed_any ? "\n" : "", (int) r.verbose.len, r.verbose.data);
                listed_any = true;
            }
            for (uint32_t s = 0; s < r.sections.num; s++) {
                rc_array_section_push(&saved, section_make_copy(rc_view_section_get(r.sections, s), &cli), &cli);
            }
        }
        else {
            failed = true;
        }
    }

    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
    rc_arena_deinit(&cli);
    return failed ? 1 : 0;
}
