#include "symdump.h"
#include "symbols.h"
#include "value.h"

#include "richc/arena.h"
#include "richc/mstr.h"


// Sort a span of entries by path, so the dump is stable across builds and reorders.
#define RC_SORT_TYPE symbol_entry
#define RC_SORT_SPAN rc_span_symbol_entry
#define RC_SORT_NAME sort_entries_by_path
#define RC_SORT_CMP(a, b) (rc_str_compare((a).path, (b).path) < 0)
#include "richc/template/algorithm/sort.h"


void symdump_append_file(rc_mstr *out, rc_str path, scopes_view v, rc_arena *arena, rc_arena scratch)
{
    // Harvest the WHOLE table - anonymous internals included - then sort a copy (the flatten
    // hands back a read-only view).
    rc_view_symbol_entry  harvested = scopes_view_flatten_all(v, arena, scratch);
    rc_array_symbol_entry entries   = rc_array_symbol_entry_make_copy(harvested, 8, &scratch);
    sort_entries_by_path(entries.span);

    // The file's path is a JSON string like any other - a synthetic string value borrows the escaper.
    rc_mstr_append(out, RC_STR("  "), arena);
    value_append_json(out, value_make_string(path), arena);
    rc_mstr_append(out, RC_STR(": {"), arena);

    for (uint32_t i = 0; i < entries.num; i++) {
        symbol_entry e = rc_array_symbol_entry_get(&entries, i);
        rc_mstr_append(out, i > 0 ? RC_STR(",\n    ") : RC_STR("\n    "), arena);
        value_append_json(out, value_make_string(e.path), arena);
        rc_mstr_append(out, RC_STR(": "), arena);
        value_append_json(out, e.v, arena);
    }

    if (entries.num != 0) {
        rc_mstr_append(out, RC_STR("\n  "), arena);
    }
    rc_mstr_append_char(out, '}', arena);
}


#ifdef BARON_TESTS

#include "assemble.h"
#include "richc/test.h"

RC_TEST(symdump, renders_sorted_json)
{
    baron_desc desc = {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };

    // Labels, a computed constant, a string, and a nested scope - the dump is one JSON member,
    // keys sorted by full path (.zip { is both a label and a scope, so it appears twice over).
    baron_result r = assemble_string(&desc, RC_STR("prog"),
                                     RC_STR("section code, org=&900\n.start\nrts\nendsection\nwidth = 8*4\n.zip {\ninner = \"hi\"\n}"));
    RC_CHECK_TRUE(r.passes != 0);

    rc_arena out_arena = rc_arena_make_default();
    rc_arena scratch   = rc_arena_make_default();
    rc_mstr  out = rc_mstr_make(64, &out_arena);
    symdump_append_file(&out, RC_STR("src/prog.6502"), r.scopes, &out_arena, scratch);
    RC_CHECK(out.view, ==,
             RC_STR("  \"src/prog.6502\": {\n"
                    "    \"start\": 2304,\n"
                    "    \"width\": 32,\n"
                    "    \"zip\": 1,\n"
                    "    \"zip.inner\": \"hi\"\n"
                    "  }"));

    rc_arena_deinit(&out_arena);
    rc_arena_deinit(&scratch);
    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
}

RC_TEST(symdump, includes_anonymous_symbols)
{
    baron_desc desc = {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };

    // A binding inside an anonymous block dumps under its unspellable '@source:pos' segment, and
    // a local label dumps under its '@' key - both invisible to the plain flatten.
    baron_result r = assemble_string(&desc, RC_STR("anon"), RC_STR("{\nhidden = 7\n}\n.@\nnop"));
    RC_CHECK_TRUE(r.passes != 0);

    rc_arena out_arena = rc_arena_make_default();
    rc_arena scratch   = rc_arena_make_default();
    rc_mstr  out = rc_mstr_make(64, &out_arena);
    symdump_append_file(&out, RC_STR("anon"), r.scopes, &out_arena, scratch);

    RC_CHECK_TRUE(rc_str_find_first(out.view, RC_STR("hidden\": 7")) != RC_INDEX_NONE);
    RC_CHECK_TRUE(rc_str_find_first(out.view, RC_STR("\"@")) != RC_INDEX_NONE);

    rc_arena_deinit(&out_arena);
    rc_arena_deinit(&scratch);
    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
}

RC_TEST(symdump, empty_table_is_an_empty_object)
{
    baron_desc desc = {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };

    baron_result r = assemble_string(&desc, RC_STR("bare"), RC_STR("nop"));
    RC_CHECK_TRUE(r.passes != 0);

    rc_arena out_arena = rc_arena_make_default();
    rc_arena scratch   = rc_arena_make_default();
    rc_mstr  out = rc_mstr_make(64, &out_arena);
    symdump_append_file(&out, RC_STR("bare"), r.scopes, &out_arena, scratch);
    RC_CHECK(out.view, ==, RC_STR("  \"bare\": {}"));

    rc_arena_deinit(&out_arena);
    rc_arena_deinit(&scratch);
    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
}

#endif // BARON_TESTS
