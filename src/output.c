#include "output.h"

#include "file_utils.h"   // file_path_join: entries land inside -p's directory

#include "richc/file.h"
#include "richc/macros.h"
#include "richc/mstr.h"


// "section 'name': <what went wrong>" - the shape every builder complaint takes.
static rc_str spec_error(rc_str name, const char *what, rc_arena *arena)
{
    rc_mstr s = rc_mstr_make(64, arena);
    rc_mstr_append(&s, RC_STR("section '"), arena);
    rc_mstr_append(&s, name, arena);
    rc_mstr_append(&s, RC_STR("': "), arena);
    rc_mstr_append(&s, rc_str_from_cstr(what), arena);
    return s.view;
}

// "cannot write 'name'" - the shape every writer complaint takes.
static rc_str write_error(rc_str filename, rc_arena *arena)
{
    rc_mstr s = rc_mstr_make(64, arena);
    rc_mstr_append(&s, RC_STR("cannot write '"), arena);
    rc_mstr_append(&s, filename, arena);
    rc_mstr_append_char(&s, '\'', arena);
    return s.view;
}

// The value of the attribute named `key` on a section, or none when absent.
static value section_attr(section s, rc_str key)
{
    for (uint32_t i = 0; i < s.attributes.num; i++) {
        attribute a = rc_array_attribute_get(&s.attributes, i);
        if (rc_str_is_equal(a.key, key)) {
            return a.v;
        }
    }
    return value_make_none();
}

// A numeric value squeezed into an address: it must be whole and fit unsigned 32 bits. `ok` false means
// it was neither (or not a number at all).
typedef struct address_result {
    uint32_t v;
    bool     ok;
} address_result;

static address_result address_from_value(value v)
{
    if (!value_is_number(v) || v.numeric < 0.0 || v.numeric > 4294967295.0) {
        return (address_result) {0};
    }
    uint32_t u = (uint32_t) v.numeric;
    if ((double) u != v.numeric) {
        return (address_result) {0};   // fractional
    }
    return (address_result) {.v = u, .ok = true};
}

output_spec_result output_spec_make(rc_view_section sections, rc_str title, uint32_t boot, uint32_t cycle,
                                    rc_arena *arena)
{
    rc_array_output_entry entries = rc_array_output_entry_make(8, arena);

    for (uint32_t i = 0; i < sections.num; i++) {
        section s = rc_view_section_get(sections, i);

        // `filename` is the opt-in: a section becomes an output by naming the file it saves to. Absent
        // means not an output (which quietly covers the nameless default sections too - they can never
        // carry attributes), and an EMPTY filename opts back out, cancelling an inherited one.
        value fname = section_attr(s, RC_STR("filename"));
        if (value_is_none(fname)) {
            continue;
        }
        if (!value_is_string(fname)) {
            return (output_spec_result) {.error = spec_error(s.name, "filename attribute must be a string", arena)};
        }
        if (fname.string.len == 0) {
            continue;
        }
        rc_str filename = fname.string;

        // load: the `load` attribute, else `org` (the section's start address), else where the code
        // started - pc ran on to the end of it, so back the length off.
        uint32_t load = s.pc - s.code.num;
        address_result org = address_from_value(section_attr(s, RC_STR("org")));
        if (org.ok) {
            load = org.v;
        }
        value lv = section_attr(s, RC_STR("load"));
        if (!value_is_none(lv)) {
            address_result a = address_from_value(lv);
            if (!a.ok) {
                return (output_spec_result) {.error = spec_error(s.name, "load attribute must be a whole number", arena)};
            }
            load = a.v;
        }

        uint32_t exec = load;
        value ev = section_attr(s, RC_STR("exec"));
        if (!value_is_none(ev)) {
            address_result a = address_from_value(ev);
            if (!a.ok) {
                return (output_spec_result) {.error = spec_error(s.name, "exec attribute must be a whole number", arena)};
            }
            exec = a.v;
        }

        rc_array_output_entry_push(
            &entries,
            (output_entry) {
                .filename = filename,
                .load     = load,
                .exec     = exec,
                .code     = s.code.view,
            },
            arena);
    }

    return (output_spec_result) {
        .spec = {
            .title   = title,
            .boot    = boot,
            .cycle   = cycle,
            .entries = entries.view,
        },
    };
}

rc_str output_write_files(const output_spec *spec, rc_str dir, bool inf, rc_arena *arena)
{
    RC_ASSERT(spec != NULL && arena != NULL);
    for (uint32_t i = 0; i < spec->entries.num; i++) {
        output_entry e = rc_view_output_entry_get(spec->entries, i);
        rc_str path = file_path_join(dir, e.filename, arena);
        if (rc_file_save_binary(path, e.code) != RC_FILE_OK) {
            return write_error(path, arena);
        }
        if (inf) {
            // The sidecar line: the DFS-style name (the filename as given when it already carries a "d."
            // directory prefix, "$." glued on otherwise), then load / exec / length in hex.
            rc_mstr line = rc_mstr_make(48, arena);
            if (!(e.filename.len >= 2 && e.filename.data[1] == '.')) {
                rc_mstr_append(&line, RC_STR("$."), arena);
            }
            rc_mstr_append(&line, e.filename, arena);
            rc_mstr_append_char(&line, ' ', arena);
            rc_mstr_append_hex32(&line, e.load, arena);
            rc_mstr_append_char(&line, ' ', arena);
            rc_mstr_append_hex32(&line, e.exec, arena);
            rc_mstr_append_char(&line, ' ', arena);
            rc_mstr_append_hex32(&line, e.code.num, arena);
            rc_mstr_append_char(&line, '\n', arena);

            rc_mstr name = rc_mstr_from_str(path, 0, arena);
            rc_mstr_append(&name, RC_STR(".inf"), arena);
            if (rc_file_save_text(name.view, line.view) != RC_FILE_OK) {
                return write_error(name.view, arena);
            }
        }
    }
    return (rc_str) {0};
}

#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(output, spec_from_sections)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    // "main": saved (a filename names the output), with an org attribute and its own exec. Its load
    // should come from org.
    uint32_t a = sections_make(&sec, RC_STR("main"));
    sections_add_attribute(&sec, a, RC_STR("filename"), value_make_string(RC_STR("main")), cursor_none());
    sections_add_attribute(&sec, a, RC_STR("org"), value_make_numeric(0x1900), cursor_none());
    sections_add_attribute(&sec, a, RC_STR("exec"), value_make_numeric(0x1903), cursor_none());
    sections_org(&sec, a, 0x1900);
    sections_emit_u8(&sec, a, 0xA9);
    sections_emit_u8(&sec, a, 0x2A);

    // "quiet": no filename attribute, so not an output at all.
    uint32_t q = sections_make(&sec, RC_STR("quiet"));
    sections_emit_u8(&sec, q, 0x60);

    // "data": saved under a directory-specified filename, no org ATTRIBUTE - load falls back to where
    // the code started (pc minus length), and exec follows load.
    uint32_t d = sections_make(&sec, RC_STR("data"));
    sections_add_attribute(&sec, d, RC_STR("filename"), value_make_string(RC_STR("X.tab")), cursor_none());
    sections_org(&sec, d, 0x2000);
    sections_emit_u8(&sec, d, 0x0D);

    // "off": filename = "" opts back out (an inherited filename can be cancelled this way).
    uint32_t off = sections_make(&sec, RC_STR("off"));
    sections_add_attribute(&sec, off, RC_STR("filename"), value_make_string(RC_STR("")), cursor_none());
    sections_emit_u8(&sec, off, 0xEA);

    output_spec_result r = output_spec_make(sections_all(&sec), RC_STR("T"), 3, 42, &arena);
    RC_CHECK(r.error.len, ==, 0u);
    RC_CHECK(r.spec.title, ==, RC_STR("T"));
    RC_CHECK(r.spec.boot, ==, 3u);
    RC_CHECK(r.spec.cycle, ==, 42u);
    RC_CHECK(r.spec.entries.num, ==, 2u);

    output_entry e0 = rc_view_output_entry_get(r.spec.entries, 0);
    RC_CHECK(e0.filename, ==, RC_STR("main"));
    RC_CHECK(e0.load, ==, 0x1900u);
    RC_CHECK(e0.exec, ==, 0x1903u);
    RC_CHECK(e0.code.num, ==, 2u);

    output_entry e1 = rc_view_output_entry_get(r.spec.entries, 1);
    RC_CHECK(e1.filename, ==, RC_STR("X.tab"));
    RC_CHECK(e1.load, ==, 0x2000u);
    RC_CHECK(e1.exec, ==, 0x2000u);
    RC_CHECK(e1.code.num, ==, 1u);

    rc_arena_deinit(&arena);
}

RC_TEST(output, spec_rejects_bad_attributes)
{
    rc_arena arena = rc_arena_make_default();
    sections sec;
    sections_init(&sec, &arena, &arena);
    sections_reset(&sec);

    uint32_t a = sections_make(&sec, RC_STR("code"));
    sections_add_attribute(&sec, a, RC_STR("filename"), value_make_numeric(7), cursor_none());
    output_spec_result r = output_spec_make(sections_all(&sec), RC_STR(""), 0, 0, &arena);
    RC_CHECK_TRUE(r.error.len != 0);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("code")));
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("filename")));

    // A fractional load is refused too (addresses are whole numbers).
    sections_add_attribute(&sec, a, RC_STR("filename"), value_make_string(RC_STR("code")), cursor_none());
    sections_add_attribute(&sec, a, RC_STR("load"), value_make_numeric(0.5), cursor_none());
    r = output_spec_make(sections_all(&sec), RC_STR(""), 0, 0, &arena);
    RC_CHECK_TRUE(r.error.len != 0);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("load")));

    rc_arena_deinit(&arena);
}

RC_TEST(output, files_and_inf_sidecars)
{
    rc_arena arena = rc_arena_make_default();
    static const uint8_t code[] = {0xA9, 0x2A, 0x60};
    output_entry e[] = {
        {.filename = RC_STR("TSTOUT"), .load = 0x1900, .exec = 0x1903, .code = RC_VIEW(code)},
    };
    output_spec spec = {.entries = RC_VIEW(e)};

    rc_str err = output_write_files(&spec, RC_STR(""), true, &arena);
    RC_CHECK(err.len, ==, 0u);

    // The binary round-trips, and the sidecar carries the addresses in the interchange shape.
    rc_file_load_binary_result bin = rc_file_load_binary(RC_STR("TSTOUT"), 0, &arena);
    RC_CHECK_TRUE(bin.error == RC_FILE_OK);
    RC_CHECK(bin.contents.num, ==, 3u);
    RC_CHECK((uint32_t) rc_array_bytes_get(&bin.contents, 0), ==, 0xA9u);
    rc_file_load_text_result inf = rc_file_load_text(RC_STR("TSTOUT.inf"), 0, &arena);
    RC_CHECK_TRUE(inf.error == RC_FILE_OK);
    RC_CHECK(inf.text.view, ==, RC_STR("$.TSTOUT 00001900 00001903 00000003\n"));

    rc_file_delete(RC_STR("TSTOUT"));
    rc_file_delete(RC_STR("TSTOUT.inf"));

    // A directory is glued on to both the binary and its sidecar; the sidecar's own DFS name is the
    // bare filename, path or no path.
    err = output_write_files(&spec, RC_STR("."), true, &arena);
    RC_CHECK(err.len, ==, 0u);
    bin = rc_file_load_binary(RC_STR("./TSTOUT"), 0, &arena);
    RC_CHECK_TRUE(bin.error == RC_FILE_OK);
    RC_CHECK(bin.contents.num, ==, 3u);
    inf = rc_file_load_text(RC_STR("./TSTOUT.inf"), 0, &arena);
    RC_CHECK_TRUE(inf.error == RC_FILE_OK);
    RC_CHECK(inf.text.view, ==, RC_STR("$.TSTOUT 00001900 00001903 00000003\n"));

    rc_file_delete(RC_STR("./TSTOUT"));
    rc_file_delete(RC_STR("./TSTOUT.inf"));
    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
