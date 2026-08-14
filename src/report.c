#include "report.h"

#include "richc/mstr.h"
#include "richc/macros.h"


line_col line_col_from_offset(rc_str text, uint32_t pos)
{
    // Tolerate a cursor sitting at (or beyond) the end of the text - errors at end-of-input point there.
    if (pos > text.len) {
        pos = text.len;
    }
    // One forward scan, counting newlines and remembering where the current line began. O(pos) per call is
    // plenty: this only ever runs while rendering diagnostics, of which a sane program has few. Columns are
    // counted in BYTES ('\n' alone ends a line; tabs and multi-byte characters count one per byte) - a
    // deliberate simplification until someone's editor complains.
    line_col lc = {.line = 1, .col = 1};
    uint32_t line_start = 0;
    for (uint32_t i = 0; i < pos; i++) {
        if (text.data[i] == '\n') {
            lc.line += 1;
            line_start = i + 1;
        }
    }
    lc.col = pos - line_start + 1;
    return lc;
}

// The label after the location: companion frames are context for the error above them, not fresh failures,
// so they read as notes whatever their severity says.
static rc_str severity_label(diagnostic d)
{
    if (d.code == error_type_original_definition ||
        d.code == error_type_included_from ||
        d.code == error_type_expanded_from) {
        return RC_STR("note");
    }
    return d.severity == severity_error ? RC_STR("error") : RC_STR("warning");
}

// One diagnostic, one line: "<name>:<line>:<col>: <label>: <message>\n". A source index we cannot resolve
// (the unreadable-root case, or a future cursor_none) falls back to the caller-supplied origin, location-free.
static void append_diagnostic(rc_mstr *out, diagnostic d, rc_view_source_file sources, rc_str origin,
                              rc_arena *arena)
{
    if (d.at.source < sources.num) {
        source_file src = rc_view_source_file_get(sources, d.at.source);
        line_col lc = line_col_from_offset(src.text, d.at.pos);
        rc_mstr_append(out, src.name, arena);
        rc_mstr_append_char(out, ':', arena);
        rc_mstr_append_u32(out, lc.line, arena);
        rc_mstr_append_char(out, ':', arena);
        rc_mstr_append_u32(out, lc.col, arena);
    }
    else {
        rc_mstr_append(out, origin, arena);
    }
    rc_mstr_append(out, RC_STR(": "), arena);
    rc_mstr_append(out, severity_label(d), arena);
    rc_mstr_append(out, RC_STR(": "), arena);
    rc_mstr_append(out, error_type_name(d.code), arena);
    rc_mstr_append_char(out, '\n', arena);
}

rc_str report_render(const baron_result *r, rc_str origin, uint8_t severity_threshold, rc_arena *arena)
{
    RC_ASSERT(r != NULL && arena != NULL);
    // The flat diagnostics array is already in narrative order - each companion note directly follows the
    // error it annotates (by construction in the assembler) - so rendering is a straight filtered pass.
    // Notes carry severity 0, so they can never be filtered away from under their error.
    rc_mstr out = {0};
    for (uint32_t i = 0; i < r->diagnostics.num; i++) {
        diagnostic d = rc_view_diagnostic_get(r->diagnostics, i);
        if (d.severity <= severity_threshold) {
            append_diagnostic(&out, d, r->sources, origin, arena);
        }
    }
    return out.view;
}

#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(report, line_col_from_offset)
{
    rc_str text = RC_STR("abc\ndef");

    line_col lc = line_col_from_offset(text, 0);        // start of everything
    RC_CHECK(lc.line, ==, 1u);
    RC_CHECK(lc.col, ==, 1u);

    lc = line_col_from_offset(text, 5);                 // 'e': mid second line
    RC_CHECK(lc.line, ==, 2u);
    RC_CHECK(lc.col, ==, 2u);

    lc = line_col_from_offset(text, 4);                 // 'd': first char after the newline
    RC_CHECK(lc.line, ==, 2u);
    RC_CHECK(lc.col, ==, 1u);

    lc = line_col_from_offset(text, 3);                 // the '\n' itself still belongs to line 1
    RC_CHECK(lc.line, ==, 1u);
    RC_CHECK(lc.col, ==, 4u);

    lc = line_col_from_offset(text, 7);                 // pos == len: one past the last char
    RC_CHECK(lc.line, ==, 2u);
    RC_CHECK(lc.col, ==, 4u);

    lc = line_col_from_offset(text, 99);                // beyond the end clamps rather than wanders off
    RC_CHECK(lc.line, ==, 2u);
    RC_CHECK(lc.col, ==, 4u);

    lc = line_col_from_offset(RC_STR(""), 0);           // an empty source still has a position 1:1
    RC_CHECK(lc.line, ==, 1u);
    RC_CHECK(lc.col, ==, 1u);
}

RC_TEST_GROUP_DATA(report) {
    baron_arenas arenas;
    baron_result r;
};

RC_TEST_GROUP_INIT(report, fix)
{
    fix->arenas = baron_arenas_make();
}

RC_TEST_GROUP_DEINIT(report, fix)
{
    baron_arenas_deinit(&fix->arenas);
}

// Assemble a snippet under an explicit name (INCLUDE resolution peels a directory off the name, so tests
// that include a file need a slash-free one) and stash the result for rendering.
#define ASM(name, src) (fix->r = assemble_string(&fix->arenas, RC_STR(name), RC_STR(src)), fix->r.passes)

// Render the stashed result at a threshold. The origin only shows for an unresolvable cursor, which these
// string-based assembles never produce - the file-based test below calls report_render itself.
#define RENDER(threshold) report_render(&fix->r, RC_STR("origin"), (threshold), &fix->arenas.permanent)

RC_TEST_STEP(report, renders_error_with_location, fix)
{
    // An undefined symbol on the second line: the rendered line must carry the source name and 1-based
    // line/col of the offending operand.
    RC_CHECK(ASM("t", "nop\nx = zork"), ==, 0u);
    RC_CHECK(RENDER(severity_warning), ==, RC_STR("t:2:4: error: Undefined symbol\n"));
}

RC_TEST_STEP(report, warning_shown_and_filtered_by_threshold, fix)
{
    // The NMOS JMP page-wrap bug is a default (level 1) warning: visible at the default threshold, gone
    // when only errors are asked for - and never a failure (the assemble still passes).
    RC_CHECK_TRUE(ASM("t", "jmp (&12FF)") != 0);
    RC_CHECK(RENDER(severity_warning), ==,
             RC_STR("t:1:4: warning: Indirect JMP vector straddles a page boundary (6502 hardware bug)\n"));
    RC_CHECK(RENDER(severity_error).len, ==, 0u);
}

RC_TEST_STEP(report, duplicate_renders_original_definition_note, fix)
{
    // A duplicate label signposts its first binding: the error at the redefinition, then a note at the
    // original - the note keeps the FIRST binding's location.
    RC_CHECK(ASM("t", ".here\n.here"), ==, 0u);
    RC_CHECK(RENDER(severity_warning), ==,
             RC_STR("t:2:2: error: Symbol is already defined in this scope\n"
                    "t:1:2: note: Originally defined here\n"));
}

RC_TEST_STEP(report, unreadable_file_renders_from_origin, fix)
{
    // An unreadable root file never registers a source, so its diagnostic has nowhere to point: the render
    // falls back to the origin - the path the caller knows the input by - with no line/col at all.
    fix->r = assemble_file(&fix->arenas, RC_STR("no_such_baron_file.6502"));
    RC_CHECK(fix->r.passes, ==, 0u);
    rc_str rep = report_render(&fix->r, RC_STR("no_such_baron_file.6502"), severity_warning,
                               &fix->arenas.permanent);
    RC_CHECK(rep, ==, RC_STR("no_such_baron_file.6502: error: Could not read the source file\n"));
}

RC_TEST_STEP(report, include_error_names_the_child_file, fix)
{
    // An error inside an INCLUDEd file points at the CHILD (its name, its line/col), with a note walking
    // back to the INCLUDE site in the parent - the two-file render is the whole point of result.sources.
    RC_CHECK(ASM("top", "nop\ninclude \"inc_bad_child.6502\""), ==, 0u);
    RC_CHECK(RENDER(severity_warning), ==,
             RC_STR("inc_bad_child.6502:1:4: error: Value out of range\n"
                    "top:2:8: note: Included from here\n"));
}

#endif // BARON_TESTS
