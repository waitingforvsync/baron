#include "assemble.h"
#include "report.h"
#include "output.h"
#include "disc_ssd.h"

#include "richc/file.h"
#include "richc/mstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BARON_TESTS
#include "richc/test.h"
#endif


static const char usage[] =
    "usage: baron [-v] [--inf] [-o <image.ssd>] [--title <t>] [--opt <0-3>] [--cycle <0-99>]"
    " [-log<n> <file>] <source files>\n";

// A decimal option value in [0, max], or -1 with a complaint printed. `what` names the switch.
static long parse_option_value(const char *what, const char *s, long max)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 0 || v > max) {
        fprintf(stderr, "baron: %s must be 0-%ld\n", what, max);
        return -1;
    }
    return v;
}

// A channel-redirect switch -log0 .. -log9, or -1. Its channel digit is the switch's own last character.
static int log_channel(const char *arg)
{
    if (strncmp(arg, "-log", 4) == 0 && arg[4] >= '0' && arg[4] <= '9' && arg[5] == '\0') {
        return arg[4] - '0';
    }
    return -1;
}

// True for a switch that consumes the following argument as its value - shared by the option pass and the
// assemble pass, which must SKIP those values or it would try to assemble them.
static bool option_takes_value(const char *arg)
{
    return strcmp(arg, "-o") == 0 || strcmp(arg, "--title") == 0
        || strcmp(arg, "--opt") == 0 || strcmp(arg, "--cycle") == 0
        || log_channel(arg) >= 0;
}

// baron [options] <list of source files>. Each file is assembled in a fresh environment - symbols never
// leak between files (the job BeebAsm's CLEAR used to do) - and its diagnostics are reported as soon as it
// finishes. Success is silent; any error anywhere makes the exit code 1 (but every file is still assembled
// first, so one run reports everything). The sections of each successful file are deep-copied into `saved`:
// they are what the output stage below writes from, kept alive here precisely because the next
// assemble_file supersedes the previous result's sections.
//
// The output stage runs once everything has assembled: with no -o, every section marked save = TRUE is
// written as a plain binary in the current directory (--inf adds a .inf sidecar carrying its addresses);
// with -o <image.ssd>, the same sections become a DFS disc image instead, in the order they were collected,
// with --title / --opt / --cycle supplying the disc-level metadata no section can know.
int main(int argc, char **argv)
{
#ifdef BARON_TESTS
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        return rc_test_run(argc > 2 ? argv[2] : "");
    }
#endif

    // Options first. A value-taking switch as the last argument falls through to the unknown-option
    // complaint (there is no value to take), and at least one real file must remain.
    bool verbose = false;
    bool inf = false;
    const char *out = NULL;
    const char *title = "";
    const char *log_paths[baron_num_channels] = {0};   // -logN: write PRINT channel N to this file
    long boot = 0;
    long cycle = 0;
    bool disc_options = false;   // any of --title/--opt/--cycle, which only mean something with -o
    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        }
        else if (strcmp(argv[i], "--inf") == 0) {
            inf = true;
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        }
        else if (log_channel(argv[i]) >= 0 && i + 1 < argc) {
            log_paths[log_channel(argv[i])] = argv[i + 1];
            i++;
        }
        else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
            disc_options = true;
        }
        else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc) {
            boot = parse_option_value("--opt", argv[++i], 3);
            disc_options = true;
        }
        else if (strcmp(argv[i], "--cycle") == 0 && i + 1 < argc) {
            cycle = parse_option_value("--cycle", argv[++i], 99);
            disc_options = true;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "baron: unknown option '%s'\n%s", argv[i], usage);
            return 1;
        }
        else {
            files++;
        }
    }
    if (boot < 0 || cycle < 0) {
        return 1;   // parse_option_value already complained
    }
    if (files == 0) {
        fprintf(stderr, "%s", usage);
        return 1;
    }
    if (out == NULL && disc_options) {
        fprintf(stderr, "baron: --title/--opt/--cycle describe a disc image and need -o\n%s", usage);
        return 1;
    }
    if (out != NULL && inf) {
        fprintf(stderr, "baron: --inf applies to loose file output only (drop -o)\n%s", usage);
        return 1;
    }
    if (out != NULL) {
        // The extension picks the writer; only DFS discs exist so far, and refusing the rest up front
        // keeps the namespace free for .adf / .uef later.
        rc_str o = rc_str_from_cstr(out);
        if (o.len < 5 || !rc_str_is_equal_insensitive(rc_str_right(o, 4), RC_STR(".ssd"))) {
            fprintf(stderr, "baron: unsupported output format '%s' (expected a .ssd image)\n", out);
            return 1;
        }
    }

    // One arena for everything the CLI itself keeps (section copies, rendered reports, the output spec and
    // image); one baron_desc - arenas plus options - REUSED across all files. Reuse is safe because nothing
    // outlives its turn: each report is printed before the next assemble supersedes the result it came
    // from, and the sections worth keeping are copied.
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

    rc_mstr logs[baron_num_channels] = {0};   // the redirected channels, accumulated across files

    for (int i = 1; i < argc; i++) {
        if (option_takes_value(argv[i])) {
            i++;        // skip the switch and its value
            continue;
        }
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
            // Channel 0 - the PRINT output, with the -v listing interleaved when asked for - goes to
            // stdout unless -log0 claims it (it is the product; diagnostics are commentary), one blank
            // line between files so a multi-file run reads as chapters. Redirected channels accumulate
            // across files and are written once at the end.
            if (log_paths[0] == NULL && r.channels[0].len != 0) {
                fprintf(stdout, "%s%.*s", listed_any ? "\n" : "", (int) r.channels[0].len, r.channels[0].data);
                listed_any = true;
            }
            for (uint32_t c = 0; c < baron_num_channels; c++) {
                if (log_paths[c] != NULL && r.channels[c].len != 0) {
                    rc_mstr_append(&logs[c], r.channels[c], &cli);
                }
            }
            for (uint32_t s = 0; s < r.sections.num; s++) {
                rc_array_section_push(&saved, section_make_copy(rc_view_section_get(r.sections, s), &cli), &cli);
            }
        }
        else {
            failed = true;
        }
    }

    // The -logN files: each redirected channel's accumulated text lands in its file. Same rule as the
    // output stage below - nothing is written unless every file assembled. A channel nothing printed to
    // still writes its (empty) file: the switch asked for the file to exist.
    if (!failed) {
        for (uint32_t c = 0; c < baron_num_channels; c++) {
            if (log_paths[c] == NULL) {
                continue;
            }
            rc_str text = logs[c].len != 0 ? logs[c].view : RC_STR("");
            if (rc_file_save_text(rc_str_from_cstr(log_paths[c]), text) != RC_FILE_OK) {
                fprintf(stderr, "baron: cannot write '%s'\n", log_paths[c]);
                failed = true;
            }
        }
    }

    // The output stage. Nothing is written unless EVERY file assembled - a partial batch would quietly
    // produce outputs with sections missing.
    if (!failed) {
        output_spec_result sr = output_spec_make(saved.view, rc_str_from_cstr(title),
                                                 (uint32_t) boot, (uint32_t) cycle, &cli);
        if (sr.error.len != 0) {
            fprintf(stderr, "baron: %.*s\n", (int) sr.error.len, sr.error.data);
            failed = true;
        }
        else if (out == NULL) {
            rc_str err = output_write_files(&sr.spec, inf, &cli);
            if (err.len != 0) {
                fprintf(stderr, "baron: %.*s\n", (int) err.len, err.data);
                failed = true;
            }
        }
        else {
            // An empty disc is still a valid disc - a bare catalogue - but it is more likely a forgotten
            // save attribute, so say so.
            if (sr.spec.entries.num == 0) {
                fprintf(stderr, "baron: warning: no sections marked save = TRUE; writing an empty disc image\n");
            }
            disc_ssd_result d = disc_ssd_make(&sr.spec, &cli);
            if (d.error.len != 0) {
                fprintf(stderr, "baron: %.*s\n", (int) d.error.len, d.error.data);
                failed = true;
            }
            else if (rc_file_save_binary(rc_str_from_cstr(out), d.image.view) != RC_FILE_OK) {
                fprintf(stderr, "baron: cannot write '%s'\n", out);
                failed = true;
            }
        }
    }

    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
    rc_arena_deinit(&cli);
    return failed ? 1 : 0;
}
