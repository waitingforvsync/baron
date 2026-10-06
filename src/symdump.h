#ifndef BARON_SYMDUMP_H_
#define BARON_SYMDUMP_H_

#include "assemble.h"


// The --symbols JSON document: one entry per assembly (a command-line source file), each listing the
// sources it read and its sections, every section carrying its attributes and the symbols defined in
// it grouped by kind:
//
//   {
//     "format": 2,
//     "assemblies": [
//       {
//         "sources": ["game.6502", "zp.6502"],
//         "sections": [
//           {
//             "name": "code", "parent": 0, "size": 3,
//             "attributes": {
//               "org": 2304
//             },
//             "labels": {
//               "start": {"value": 2304, "source": 0, "line": 2}
//             }
//           }
//         ]
//       }
//     ]
//   }
//
// Indices are local to their assembly: "parent" indexes its sections, "source" its sources. Every
// symbol appears, unspellable '@' internals included, sorted by full path within its group and one
// per line, so a grep for '"name"' answers with its value.

// Append r's entry to the assemblies being gathered in out, after a separating comma if out already
// holds one. r is read immediately - nothing of it is kept.
void symdump_append_assembly(rc_mstr *out, const baron_result *r, rc_arena *arena, rc_arena scratch);

// The whole document around the gathered assemblies.
rc_str symdump_document(rc_str assemblies, rc_arena *arena);


#endif // ifndef BARON_SYMDUMP_H_
