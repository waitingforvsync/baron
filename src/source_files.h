#ifndef BARON_SOURCE_FILES_H_
#define BARON_SOURCE_FILES_H_

#include "richc/str.h"     // rc_str
#include "richc/arena.h"   // rc_arena
#include <stdint.h>


// One cached source: its name (the path it was loaded from, or a caller-given name for a source
// supplied directly as a string) and its full contents as an rc_str. Like a scope or a section,
// this is an internal element of its manager - reached by index, never by pointer.
typedef struct source_file {
    rc_str name;
    rc_str text;
} source_file;

#define RC_ARRAY_TYPE source_file
#define RC_ARRAY_NAME source_file
#include "richc/template/array.h"


// The source-file cache: a manager of cached sources addressed by index, in the same spirit as
// `scopes` and `sections`. It owns the cached names and contents (in its own arenas), so a source
// added from a transient string or path stays valid for the manager's lifetime.
typedef struct source_files {
    rc_arena            *arena;   // BORROWED: baron's permanent arena, backs the nodes AND the cached names/text
    rc_array_source_file nodes;
} source_files;

void source_files_init(source_files *sf, rc_arena *permanent);

// Both add functions are keyed by name: if a source is already registered under the name, its
// existing index is returned and nothing is added (the cache holds one entry per name).

// Load a file's text (richc whole-file load) and cache it under its path name; returns its index,
// or RC_INDEX_NONE if the file is not already cached and could not be read.
uint32_t source_files_add_file(source_files *sf, rc_str path);

// Cache a source supplied directly as text, under the given name; returns its index. The name and
// the text are copied (when newly added), so neither needs to outlive the call.
uint32_t source_files_add_string(source_files *sf, rc_str name, rc_str text);

// The index of the cached source with this name, or RC_INDEX_NONE if none matches.
uint32_t source_files_find(const source_files *sf, rc_str name);

// The cached contents / name at an index.
rc_str source_files_text(const source_files *sf, uint32_t index);
rc_str source_files_name(const source_files *sf, uint32_t index);


#endif // ifndef BARON_SOURCE_FILES_H_
