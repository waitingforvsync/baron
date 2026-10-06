#include "symdump.h"
#include "symbols.h"
#include "value.h"

#include "richc/arena.h"
#include "richc/mstr.h"
#include "richc/array/u32.h"


// Dump order: by section, then kind (the group order), then full path.
static bool entry_precedes(symbol_entry a, symbol_entry b)
{
    if (a.sym.section != b.sym.section) {
        return a.sym.section < b.sym.section;
    }
    if (a.sym.kind != b.sym.kind) {
        return a.sym.kind < b.sym.kind;
    }
    return rc_str_compare(a.path, b.path) < 0;
}

#define RC_SORT_TYPE symbol_entry
#define RC_SORT_SPAN rc_span_symbol_entry
#define RC_SORT_NAME sort_entries
#define RC_SORT_CMP(a, b) entry_precedes(a, b)
#include "richc/template/algorithm/sort.h"

#define RC_LOWER_BOUND_TYPE uint32_t
#define RC_LOWER_BOUND_VIEW rc_view_u32
#define RC_LOWER_BOUND_NAME lower_bound_u32
#include "richc/template/algorithm/lower_bound.h"


// Every source's '\n' offsets back to back, so finding a definition's line is a binary search rather
// than a rescan of its file per symbol.
typedef struct line_index {
    rc_view_u32 newlines;
    rc_view_u32 first;   // source s owns newlines [first[s], first[s + 1])
} line_index;

static line_index line_index_make(rc_view_source_file sources, rc_arena *arena)
{
    rc_array_u32 newlines = rc_array_u32_make(1024, arena);
    rc_array_u32 first    = rc_array_u32_make(sources.num + 1, arena);
    for (uint32_t s = 0; s < sources.num; s++) {
        rc_array_u32_push(&first, newlines.num, arena);
        rc_str text = rc_view_source_file_get(sources, s).text;
        for (uint32_t i = 0; i < text.len; i++) {
            if (text.data[i] == '\n') {
                rc_array_u32_push(&newlines, i, arena);
            }
        }
    }
    rc_array_u32_push(&first, newlines.num, arena);

    return (line_index) {.newlines = newlines.view, .first = first.view};
}

// The 1-based line of pos in source s: one more than the newlines before it.
static uint32_t line_of(line_index li, uint32_t s, uint32_t pos)
{
    rc_view_u32 own = rc_view_u32_get_subview(li.newlines, rc_view_u32_get(li.first, s), rc_view_u32_get(li.first, s + 1));
    return lower_bound_u32(own, pos) + 1;
}


// The key each kind's group is listed under.
static rc_str group_name(symbol_kind kind)
{
    switch (kind) {
        case symbol_kind_label:      return RC_STR("labels");
        case symbol_kind_assignment: return RC_STR("assignments");
        case symbol_kind_define:     return RC_STR("defines");
        case symbol_kind_za_auto:    return RC_STR("za_autos");
        case symbol_kind_loop_var:   return RC_STR("loop_vars");
        case symbol_kind_param:      return RC_STR("params");
    }

    RC_UNREACHABLE();
}

// A JSON string - a synthetic string value borrows the escaper.
static void append_string(rc_mstr *out, rc_str s, rc_arena *arena)
{
    value_append_json(out, value_make_string(s), arena);
}

// One symbol on its own line: "path": {"value": V, "source": S, "line": L}
static void append_symbol(rc_mstr *out, symbol_entry e, line_index li, rc_arena *arena)
{
    rc_mstr_append(out, RC_STR("            "), arena);
    append_string(out, e.path, arena);
    rc_mstr_append(out, RC_STR(": {\"value\": "), arena);
    value_append_json(out, e.sym.v, arena);
    rc_mstr_append(out, RC_STR(", \"source\": "), arena);
    rc_mstr_append_u32(out, e.sym.def.source, arena);
    rc_mstr_append(out, RC_STR(", \"line\": "), arena);
    rc_mstr_append_u32(out, line_of(li, e.sym.def.source, e.sym.def.pos), arena);
    rc_mstr_append_char(out, '}', arena);
}

// One section: its header line, then its attributes and a group per kind, each left out when empty.
// entries are this section's symbols alone, already in dump order.
static void append_section(rc_mstr *out, section s, rc_view_symbol_entry entries, line_index li, rc_arena *arena)
{
    rc_mstr_append(out, RC_STR("        {\n          "), arena);
    if (s.name.len != 0) {
        rc_mstr_append(out, RC_STR("\"name\": "), arena);
        append_string(out, s.name, arena);
        rc_mstr_append(out, RC_STR(", "), arena);
    }
    rc_mstr_append(out, RC_STR("\"parent\": "), arena);
    if (s.parent == RC_INDEX_NONE) {
        rc_mstr_append(out, RC_STR("null"), arena);
    }
    else {
        rc_mstr_append_u32(out, s.parent, arena);
    }
    rc_mstr_append(out, RC_STR(", \"size\": "), arena);
    rc_mstr_append_u32(out, s.code.num, arena);

    // Attributes as written, in source order: keys are free-form, so they get their own object rather
    // than sharing the section's keys (a repeated key replaces the earlier one, so none clash)
    if (s.attributes.num != 0) {
        rc_mstr_append(out, RC_STR(",\n          \"attributes\": {\n"), arena);
        for (uint32_t i = 0; i < s.attributes.num; i++) {
            attribute a = rc_array_attribute_get(&s.attributes, i);
            rc_mstr_append(out, i > 0 ? RC_STR(",\n            ") : RC_STR("            "), arena);
            append_string(out, a.key, arena);
            rc_mstr_append(out, RC_STR(": "), arena);
            value_append_json(out, a.v, arena);
        }
        rc_mstr_append(out, RC_STR("\n          }"), arena);
    }

    // The entries arrive grouped by kind, so a group opens wherever the kind changes
    for (uint32_t i = 0; i < entries.num; i++) {
        symbol_entry e = rc_view_symbol_entry_get(entries, i);
        bool opens = i == 0 || rc_view_symbol_entry_get(entries, i - 1).sym.kind != e.sym.kind;
        if (opens) {
            if (i > 0) {
                rc_mstr_append(out, RC_STR("\n          }"), arena);
            }
            rc_mstr_append(out, RC_STR(",\n          "), arena);
            append_string(out, group_name((symbol_kind) e.sym.kind), arena);
            rc_mstr_append(out, RC_STR(": {\n"), arena);
        }
        else {
            rc_mstr_append(out, RC_STR(",\n"), arena);
        }
        append_symbol(out, e, li, arena);
    }
    if (entries.num != 0) {
        rc_mstr_append(out, RC_STR("\n          }"), arena);
    }

    rc_mstr_append(out, RC_STR("\n        }"), arena);
}


void symdump_append_assembly(rc_mstr *out, const baron_result *r, rc_arena *arena, rc_arena scratch)
{
    // Harvest the WHOLE table - anonymous internals included - then sort a copy (the flatten hands
    // back a read-only view)
    rc_view_symbol_entry  harvested = scopes_view_flatten_all(r->scopes, arena, scratch);
    rc_array_symbol_entry entries   = rc_array_symbol_entry_make_copy(harvested, 8, &scratch);
    sort_entries(entries.span);
    line_index li = line_index_make(r->sources, &scratch);

    if (out->len != 0) {
        rc_mstr_append(out, RC_STR(",\n"), arena);
    }
    rc_mstr_append(out, RC_STR("    {\n      \"sources\": ["), arena);
    for (uint32_t i = 0; i < r->sources.num; i++) {
        if (i > 0) {
            rc_mstr_append(out, RC_STR(", "), arena);
        }
        append_string(out, rc_view_source_file_get(r->sources, i).name, arena);
    }
    rc_mstr_append(out, RC_STR("],\n      \"sections\": [\n"), arena);

    // Each section takes the run of entries carrying its index. A binding left behind by an earlier
    // pass can name a section the final pass never made; it sorts past the end and is not listed.
    uint32_t next = 0;
    for (uint32_t s = 0; s < r->sections.num; s++) {
        uint32_t begin = next;
        while (next < entries.num && rc_array_symbol_entry_get(&entries, next).sym.section == s) {
            next++;
        }
        if (s > 0) {
            rc_mstr_append(out, RC_STR(",\n"), arena);
        }
        append_section(out, rc_view_section_get(r->sections, s),
                       rc_view_symbol_entry_get_subview(entries.view, begin, next), li, arena);
    }

    rc_mstr_append(out, RC_STR("\n      ]\n    }"), arena);
}


rc_str symdump_document(rc_str assemblies, rc_arena *arena)
{
    rc_mstr doc = rc_mstr_make(assemblies.len + 64, arena);
    rc_mstr_append(&doc, RC_STR("{\n  \"format\": 2,\n  \"assemblies\": [\n"), arena);
    rc_mstr_append(&doc, assemblies, arena);
    rc_mstr_append(&doc, RC_STR("\n  ]\n}\n"), arena);
    return doc.view;
}


#ifdef BARON_TESTS

#include "richc/test.h"

// One program's whole document, built the way the CLI does: assemble, append, wrap.
typedef struct dump_fixture {
    baron_desc desc;
    rc_arena   out_arena;
    rc_arena   scratch;
    rc_mstr    assemblies;
} dump_fixture;

static dump_fixture dump_fixture_make(void)
{
    return (dump_fixture) {
        .desc = {
            .permanent = rc_arena_make_default(),
            .per_pass  = rc_arena_make_default(),
            .scratch   = rc_arena_make_default(),
        },
        .out_arena = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };
}

static bool dump_append(dump_fixture *f, rc_str name, rc_str text)
{
    baron_result r = assemble_string(&f->desc, name, text);
    if (r.passes == 0) {
        return false;
    }
    symdump_append_assembly(&f->assemblies, &r, &f->out_arena, f->scratch);
    return true;
}

static rc_str dump_document(dump_fixture *f)
{
    return symdump_document(f->assemblies.view, &f->out_arena);
}

static bool dump_contains(dump_fixture *f, rc_str needle)
{
    return rc_str_find_first(dump_document(f), needle) != RC_INDEX_NONE;
}

static void dump_fixture_deinit(dump_fixture *f)
{
    rc_arena_deinit(&f->out_arena);
    rc_arena_deinit(&f->scratch);
    rc_arena_deinit(&f->desc.permanent);
    rc_arena_deinit(&f->desc.per_pass);
    rc_arena_deinit(&f->desc.scratch);
}

RC_TEST(symdump, renders_the_document)
{
    dump_fixture f = dump_fixture_make();

    // A nested pair of sections: outer rephases to &2000 and names a file, inner rephases again. The
    // labels land in whichever section was open, and the default keeps the assignment and the tail.
    RC_CHECK_TRUE(dump_append(&f, RC_STR("prog"),
                              RC_STR("width = 8*4\n"
                                     "SECTION outer, org=&2000, filename=\"OUT\"\n"
                                     ".start LDA #width\n"
                                     "SECTION inner, org=&400\n"
                                     ".here RTS\n"
                                     "ENDSECTION\n"
                                     ".after RTS\n"
                                     "ENDSECTION\n"
                                     ".tail")));
    RC_CHECK(dump_document(&f), ==,
             RC_STR("{\n"
                    "  \"format\": 2,\n"
                    "  \"assemblies\": [\n"
                    "    {\n"
                    "      \"sources\": [\"prog\"],\n"
                    "      \"sections\": [\n"
                    "        {\n"
                    "          \"parent\": null, \"size\": 4,\n"
                    "          \"labels\": {\n"
                    "            \"tail\": {\"value\": 4, \"source\": 0, \"line\": 9}\n"
                    "          },\n"
                    "          \"assignments\": {\n"
                    "            \"width\": {\"value\": 32, \"source\": 0, \"line\": 1}\n"
                    "          }\n"
                    "        },\n"
                    "        {\n"
                    "          \"name\": \"outer\", \"parent\": 0, \"size\": 4,\n"
                    "          \"attributes\": {\n"
                    "            \"org\": 8192,\n"
                    "            \"filename\": \"OUT\"\n"
                    "          },\n"
                    "          \"labels\": {\n"
                    "            \"after\": {\"value\": 8195, \"source\": 0, \"line\": 7},\n"
                    "            \"start\": {\"value\": 8192, \"source\": 0, \"line\": 3}\n"
                    "          }\n"
                    "        },\n"
                    "        {\n"
                    "          \"name\": \"inner\", \"parent\": 1, \"size\": 1,\n"
                    "          \"attributes\": {\n"
                    "            \"org\": 1024\n"
                    "          },\n"
                    "          \"labels\": {\n"
                    "            \"here\": {\"value\": 1024, \"source\": 0, \"line\": 5}\n"
                    "          }\n"
                    "        }\n"
                    "      ]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n"));

    dump_fixture_deinit(&f);
}

RC_TEST(symdump, assemblies_stay_separate)
{
    dump_fixture f = dump_fixture_make();

    // Two files, two worlds: each numbers its own sources and sections from zero, and an empty
    // section is just its header
    RC_CHECK_TRUE(dump_append(&f, RC_STR("one"), RC_STR("x = 1")));
    RC_CHECK_TRUE(dump_append(&f, RC_STR("two"), RC_STR("nop")));
    RC_CHECK(dump_document(&f), ==,
             RC_STR("{\n"
                    "  \"format\": 2,\n"
                    "  \"assemblies\": [\n"
                    "    {\n"
                    "      \"sources\": [\"one\"],\n"
                    "      \"sections\": [\n"
                    "        {\n"
                    "          \"parent\": null, \"size\": 0,\n"
                    "          \"assignments\": {\n"
                    "            \"x\": {\"value\": 1, \"source\": 0, \"line\": 1}\n"
                    "          }\n"
                    "        }\n"
                    "      ]\n"
                    "    },\n"
                    "    {\n"
                    "      \"sources\": [\"two\"],\n"
                    "      \"sections\": [\n"
                    "        {\n"
                    "          \"parent\": null, \"size\": 1\n"
                    "        }\n"
                    "      ]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n"));

    dump_fixture_deinit(&f);
}

RC_TEST(symdump, groups_every_kind)
{
    dump_fixture f = dump_fixture_make();

    // One of each kind, the define through the descriptor like the CLI's -D
    rc_str defines[] = {RC_STR_INIT("dbg=1")};
    f.desc.defines = (rc_view_str) RC_VIEW(defines);
    RC_CHECK_TRUE(dump_append(&f, RC_STR("kinds"),
                              RC_STR("ZA_POOL &70..&7F : ZA_AUTO1 ptr\n"
                                     "MACRO poke n\n"
                                     "LDA #n : STA ptr\n"
                                     "ENDMACRO\n"
                                     "FUNCTION twice(v) = v * 2\n"
                                     "FOR i = 0..0 : NEXT\n"
                                     ".go poke twice(3)\n"
                                     "RTS")));

    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"sources\": [\"kinds\", \"-D dbg=1\"]")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"labels\": {\n            \"go\": {\"value\": 0, \"source\": 0, \"line\": 7}")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"defines\": {\n            \"dbg\": {\"value\": 1, \"source\": 1, \"line\": 1}")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"za_autos\": {\n            \"ptr\": {\"value\": 112, \"source\": 0, \"line\": 1}")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"loop_vars\": {\n")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR(".i\": {\"value\": 0, \"source\": 0, \"line\": 6}")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"params\": {\n")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR(".n\": {\"value\": 6, \"source\": 0, \"line\": 7}")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR(".v\": {\"value\": 3, \"source\": 0, \"line\": 7}")));

    dump_fixture_deinit(&f);
}

RC_TEST(symdump, include_reports_its_own_source)
{
    dump_fixture f = dump_fixture_make();

    // The included file's label is defined there, not in the file doing the including
    RC_CHECK_TRUE(dump_append(&f, RC_STR("top"), RC_STR("nop\ninclude \"inc_child.6502\"")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"sources\": [\"top\", \"inc_child.6502\"]")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"child\": {\"value\": 1, \"source\": 1, \"line\": 1}")));

    dump_fixture_deinit(&f);
}

RC_TEST(symdump, section_survives_renumbering)
{
    dump_fixture f = dump_fixture_make();

    // Pass 1 cannot see fwd, so code is section 1; pass 2 opens early first and code becomes 2. The
    // label's value never moves, but it must still follow its section to the new index.
    RC_CHECK_TRUE(dump_append(&f, RC_STR("renumber"),
                              RC_STR("IF fwd\n"
                                     "SECTION early\n"
                                     "ENDSECTION\n"
                                     "ENDIF\n"
                                     "SECTION code, org=&900\n"
                                     ".start RTS\n"
                                     "ENDSECTION\n"
                                     "fwd = 1")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"name\": \"early\", \"parent\": 0, \"size\": 0\n")));
    RC_CHECK_TRUE(dump_contains(&f, RC_STR("\"name\": \"code\", \"parent\": 0, \"size\": 1,\n"
                                           "          \"attributes\": {\n"
                                           "            \"org\": 2304\n"
                                           "          },\n"
                                           "          \"labels\": {\n"
                                           "            \"start\": {\"value\": 2304, \"source\": 0, \"line\": 6}")));

    dump_fixture_deinit(&f);
}

#endif // BARON_TESTS
