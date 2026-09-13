#ifndef BARON_SYMDUMP_H_
#define BARON_SYMDUMP_H_

#include "scopes.h"


// Append one assembled file's symbol table to a JSON document being built in out, as a single
//
//   "path": {
//     "name": value,
//     ...
//   }
//
// member - no trailing comma or outer braces, the caller frames the document. Every symbol
// appears, unspellable '@' internals included, sorted by full path so two builds diff cleanly,
// one symbol per line so a grep for '"name"' answers with its value. Values render via
// value_append_json; entries land transiently in arena alongside out.
void symdump_append_file(rc_mstr *out, rc_str path, scopes_view v, rc_arena *arena, rc_arena scratch);


#endif // ifndef BARON_SYMDUMP_H_
