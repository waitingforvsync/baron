#include "source_files.h"

#include "richc/file.h"    // rc_file_load_text
#include "richc/mstr.h"    // rc_mstr_from_str
#include "richc/macros.h"


void source_files_init(source_files *sf)
{
    RC_ASSERT(sf != NULL);
    sf->node_arena = rc_arena_make_default();
    sf->text_arena = rc_arena_make_default();
    sf->nodes      = rc_array_source_file_make(0, &sf->node_arena);
}

void source_files_deinit(source_files *sf)
{
    RC_ASSERT(sf != NULL);
    rc_arena_deinit(&sf->node_arena);
    rc_arena_deinit(&sf->text_arena);
}

// Push a node whose name/text bytes already live in text_arena.
static uint32_t add(source_files *sf, rc_str name, rc_str text)
{
    return rc_array_source_file_push(&sf->nodes, (source_file) { .name = name, .text = text }, &sf->node_arena);
}

uint32_t source_files_add_file(source_files *sf, rc_str path)
{
    RC_ASSERT(sf != NULL);
    uint32_t existing = source_files_find(sf, path);
    if (existing != RC_INDEX_NONE) {
        return existing;                   // already cached under this path - do not re-read it
    }
    rc_file_load_text_result loaded = rc_file_load_text(path, 0, &sf->text_arena);
    if (loaded.error != RC_FILE_OK) {
        return RC_INDEX_NONE;
    }
    // Own the name too, so a transient path stays valid for the manager's lifetime.
    rc_str name = rc_mstr_from_str(path, path.len, &sf->text_arena).view;
    return add(sf, name, loaded.text.view);
}

uint32_t source_files_add_string(source_files *sf, rc_str name, rc_str text)
{
    RC_ASSERT(sf != NULL);
    uint32_t existing = source_files_find(sf, name);
    if (existing != RC_INDEX_NONE) {
        return existing;                   // a source is already registered under this name
    }
    rc_str owned_name = rc_mstr_from_str(name, name.len, &sf->text_arena).view;
    rc_str owned_text = rc_mstr_from_str(text, text.len, &sf->text_arena).view;
    return add(sf, owned_name, owned_text);
}

uint32_t source_files_find(const source_files *sf, rc_str name)
{
    RC_ASSERT(sf != NULL);
    for (uint32_t i = 0; i < sf->nodes.num; i++) {
        if (rc_str_is_equal(RC_AT(sf->nodes, i).name, name)) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

rc_str source_files_text(const source_files *sf, uint32_t index)
{
    RC_ASSERT(sf != NULL);
    return RC_AT(sf->nodes, index).text;
}

rc_str source_files_name(const source_files *sf, uint32_t index)
{
    RC_ASSERT(sf != NULL);
    return RC_AT(sf->nodes, index).name;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(source_files, add_string)
{
    source_files sf;
    source_files_init(&sf);

    uint32_t i = source_files_add_string(&sf, RC_STR("inline"), RC_STR("lda #0"));
    RC_CHECK(i, ==, 0u);
    RC_CHECK(source_files_text(&sf, i), ==, RC_STR("lda #0"));
    RC_CHECK(source_files_name(&sf, i), ==, RC_STR("inline"));
    RC_CHECK(source_files_find(&sf, RC_STR("inline")), ==, 0u);
    RC_CHECK(source_files_find(&sf, RC_STR("foo")), ==, RC_INDEX_NONE);

    source_files_deinit(&sf);
}

RC_TEST(source_files, add_file_and_find)
{
    // src/test/source_sample.txt (copied next to the tests by CMake) holds exactly "rts".
    rc_str path = RC_STR("source_sample.txt");
    source_files sf;
    source_files_init(&sf);

    uint32_t i = source_files_add_file(&sf, path);
    RC_CHECK(i, ==, 0u);
    RC_CHECK(source_files_text(&sf, i), ==, RC_STR("rts"));
    RC_CHECK(source_files_name(&sf, i), ==, path);
    RC_CHECK(source_files_find(&sf, path), ==, 0u);
    RC_CHECK(source_files_find(&sf, RC_STR("nope")), ==, RC_INDEX_NONE);

    source_files_deinit(&sf);
}

RC_TEST(source_files, add_dedups_by_name)
{
    source_files sf;
    source_files_init(&sf);

    uint32_t a = source_files_add_string(&sf, RC_STR("dup"), RC_STR("one"));
    uint32_t b = source_files_add_string(&sf, RC_STR("dup"), RC_STR("two"));   // same name -> same index
    RC_CHECK(a, ==, b);
    RC_CHECK(source_files_text(&sf, a), ==, RC_STR("one"));                    // first registration wins

    source_files_deinit(&sf);
}

RC_TEST(source_files, missing_file)
{
    source_files sf;
    source_files_init(&sf);
    RC_CHECK(source_files_add_file(&sf, RC_STR("no_such_baron_file.6502")), ==, RC_INDEX_NONE);
    source_files_deinit(&sf);
}

#endif // BARON_TESTS
