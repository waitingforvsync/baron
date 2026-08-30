#include "file_utils.h"

#include "richc/mstr.h"


rc_str file_path_normalize(rc_str path, rc_arena *arena)
{
    // Copy first, then rewrite the copy - we must not scribble on the caller's (possibly read-only) view.
    rc_mstr m = rc_mstr_from_str(path, path.len, arena);
    rc_mstr_replace(&m, RC_STR("\\"), RC_STR("/"), arena);
    return m.view;
}


rc_str file_path_resolve(rc_str base, rc_str include, rc_arena *arena)
{
    rc_str base_n   = file_path_normalize(base, arena);
    rc_str included = file_path_normalize(include, arena);

    // The directory is everything up to and including base's last '/'. With no '/', base is a bare leaf
    // name, so the directory is empty and the include stands on its own.
    uint32_t slash = rc_str_find_last(base_n, RC_STR("/"));
    uint32_t cut   = (slash == RC_INDEX_NONE) ? 0 : slash + 1;

    rc_mstr m = rc_mstr_from_str(rc_str_left(base_n, cut), cut + included.len, arena);
    rc_mstr_append(&m, included, arena);
    return m.view;
}


rc_str file_path_join(rc_str dir, rc_str name, rc_arena *arena)
{
    rc_str dir_n  = file_path_normalize(dir, arena);
    rc_str name_n = file_path_normalize(name, arena);

    if (dir_n.len == 0) {
        return name_n;
    }

    rc_mstr m = rc_mstr_from_str(dir_n, dir_n.len + name_n.len + 1, arena);
    if (dir_n.data[dir_n.len - 1] != '/') {
        rc_mstr_append_char(&m, '/', arena);
    }

    rc_mstr_append(&m, name_n, arena);
    return m.view;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(file_utils, normalize)
{
    rc_arena a = rc_arena_make_default();
    RC_CHECK(file_path_normalize(RC_STR("a\\b\\c"), &a), ==, RC_STR("a/b/c"));
    RC_CHECK(file_path_normalize(RC_STR("plain"),   &a), ==, RC_STR("plain"));
    rc_arena_deinit(&a);
}

RC_TEST(file_utils, resolve)
{
    rc_arena a = rc_arena_make_default();
    // A bare base name has no directory, so the include stands alone.
    RC_CHECK(file_path_resolve(RC_STR("main.6502"), RC_STR("sub.6502"), &a), ==, RC_STR("sub.6502"));
    // A base with a directory lends it to the include.
    RC_CHECK(file_path_resolve(RC_STR("dir/main.6502"), RC_STR("sub.6502"), &a), ==, RC_STR("dir/sub.6502"));
    // The include may carry its own subdirectory, still relative to base's directory.
    RC_CHECK(file_path_resolve(RC_STR("dir/main.6502"), RC_STR("inc/child.6502"), &a), ==, RC_STR("dir/inc/child.6502"));
    // Backslashes on either side are flattened.
    RC_CHECK(file_path_resolve(RC_STR("a\\b\\main.6502"), RC_STR("x.6502"), &a), ==, RC_STR("a/b/x.6502"));
    RC_CHECK(file_path_resolve(RC_STR("dir/main.6502"), RC_STR("x\\y.6502"), &a), ==, RC_STR("dir/x/y.6502"));
    rc_arena_deinit(&a);
}

RC_TEST(file_utils, join)
{
    rc_arena a = rc_arena_make_default();
    // No directory means the name stands alone; one with or without a trailing slash gets exactly one.
    RC_CHECK(file_path_join(RC_STR(""),      RC_STR("CODE"), &a), ==, RC_STR("CODE"));
    RC_CHECK(file_path_join(RC_STR("out"),   RC_STR("CODE"), &a), ==, RC_STR("out/CODE"));
    RC_CHECK(file_path_join(RC_STR("out/"),  RC_STR("CODE"), &a), ==, RC_STR("out/CODE"));
    RC_CHECK(file_path_join(RC_STR("a\\b"),  RC_STR("CODE"), &a), ==, RC_STR("a/b/CODE"));
    RC_CHECK(file_path_join(RC_STR("/tmp/"), RC_STR("$.CODE"), &a), ==, RC_STR("/tmp/$.CODE"));
    rc_arena_deinit(&a);
}

#endif // BARON_TESTS
