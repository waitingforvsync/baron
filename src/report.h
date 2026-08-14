#ifndef BARON_REPORT_H_
#define BARON_REPORT_H_

#include "assemble.h"   // baron_result, diagnostic, severity_*; brings rc_str + rc_arena


// Rendering a result's diagnostics for human eyes. Everything here is pure string work into a caller
// arena - nothing touches stdout, so the whole module is testable and the CLI decides where the text
// actually goes.


// A 1-based line and column, computed on demand from a source text and byte offset. The assembler thinks
// in byte offsets throughout (that is what a cursor holds); humans and their editors stubbornly refuse to,
// so this is the one place that converts back.
typedef struct line_col {
    uint32_t line;   // 1-based
    uint32_t col;    // 1-based, counted in BYTES ('\n' is the only line terminator we recognise)
} line_col;

line_col line_col_from_offset(rc_str text, uint32_t pos);

// Render every diagnostic at or below the severity threshold (severity_warning shows errors plus default
// warnings; severity_error just the errors), GCC-style, one per line:
//
//   <name>:<line>:<col>: error: <message>
//   <name>:<line>:<col>: warning: <message>
//   <name>:<line>:<col>: note: <message>      (the companion frames: original definition / included from /
//                                              expanded from - context for the error above them)
//
// A cursor whose source index is not in r->sources - the unreadable-root-file case, where there was never a
// source to register - renders location-free as "<origin>: error: <message>"; `origin` is whatever name the
// caller knows the input by (the command-line path). An empty return means nothing to report at this
// threshold.
rc_str report_render(const baron_result *r, rc_str origin, uint8_t severity_threshold, rc_arena *arena);


#endif // ifndef BARON_REPORT_H_
