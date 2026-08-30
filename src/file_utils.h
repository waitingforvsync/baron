#ifndef BARON_FILE_UTILS_H_
#define BARON_FILE_UTILS_H_

#include "richc/str.h"
#include "richc/arena.h"


// Small path-string helpers for INCLUDE. Pure string work - nothing here touches the filesystem;
// opening the file is source_files' job. Both functions arena-allocate and copy their input, so a view
// into read-only source text is safe to pass in.

// A path with every backslash flattened to a forward slash (Baron's one true separator). Fresh copy.
rc_str file_path_normalize(rc_str path, rc_arena *arena);

// Where an INCLUDE actually points: include taken relative to the directory of base (the file doing
// the including). We lop off base's leaf name - everything after its last '/' - and glue the include on
// in its place; both sides are normalised first, so a mix of slash styles still lines up. Absolute
// includes are not special-cased yet, they just get appended like anything else. Fresh copy.
rc_str file_path_resolve(rc_str base, rc_str include, rc_arena *arena);

// name placed inside the directory dir - the separator is added unless dir already ends in one, and
// an empty dir leaves the name where it stands (the current directory). Both sides are normalised
// first. Fresh copy.
rc_str file_path_join(rc_str dir, rc_str name, rc_arena *arena);


#endif // ifndef BARON_FILE_UTILS_H_
