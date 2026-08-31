#ifndef BARON_ASSEMBLE_INTERNAL_H_
#define BARON_ASSEMBLE_INTERNAL_H_

#include "assemble.h"
#include "value.h"
#include "cursor.h"
#include "expression.h"


// The assembler's internal parsing vocabulary, shared between assemble.c (the statement loop and
// the directives) and opcodes.c (instruction parsing). Not part of the public assemble.h surface.

typedef struct baron baron;

// Per-statement parse context, threaded by value alongside the cursor.
typedef struct parse_flags {
    bool final;     // the single armed pass after convergence: diagnostics record, the zp IR fills
    bool active;    // false inside a dead IF/FOR branch: parse for extent, effect nothing
    bool output;    // the post-allocation re-emission pass: ZA_AUTO symbols hold their real addresses
    bool listing;   // build the -v listing text (rides on the output pass; implies output)
} parse_flags;

// The outputs of a parse, folded up by callers. Errors do not ride here - they are recorded
// straight into b->diagnostics at the failure site.
typedef struct parse_result {
    uint32_t next;         // cursor past what was consumed
    bool     fatal;        // a syntax error broke the token stream: the whole assemble unwinds
    bool     unresolved;   // some operand referenced a not-yet-defined symbol
    bool     changed;      // some existing symbol moved value
} parse_result;

// Record a fatal (syntax) error and yield the unwinding result; fires on any pass, dead branches
// included. The _payload variant attaches the string substituted for '%' in the message.
parse_result syntax_error(baron *b, error_type code, cursor at);
parse_result syntax_error_payload(baron *b, error_type code, cursor at, rc_str payload);

// Record a recoverable (semantic) error - only when flags.final && flags.active, so settling
// passes and dead branches stay quiet. The caller carries on; the assemble fails at the end.
void semantic_error(baron *b, parse_flags flags, error_type code, cursor at);
void semantic_error_payload(baron *b, parse_flags flags, error_type code, cursor at, rc_str payload);

// Like semantic_error at a positive severity level; does not fail the assemble.
void semantic_warning(baron *b, parse_flags flags, error_type code, cursor at, uint8_t severity);

// Discriminates an int_argument.
typedef enum int_argument_type {
    int_argument_type_error,        // 0/default: a value that can never be an address (fail-safe)
    int_argument_type_known,        // value is a resolved integer
    int_argument_type_unresolved,   // a forward reference - defer to a later pass
} int_argument_type;

// A value reduced to an integer argument for emission (int_argument_make, pure); a forward
// reference is unresolved on settling passes and an error on the final one.
typedef struct int_argument {
    int64_t  value;          // valid when type == int_argument_type_known
    uint8_t  type;           // int_argument_type
    uint16_t error;          // error_type, set when type == int_argument_type_error
    uint32_t error_at;
    rc_str   error_detail;   // the error's payload (e.g. the undefined symbol's name), or {0}
    bool     za_auto;        // a ZA_AUTO address: value is the offset within it, zp_* its identity
    uint32_t zp_scope;
    cursor   zp_def;
    rc_str   zp_name;
} int_argument;

int_argument int_argument_make(value v, bool final_pass, uint32_t at);

// A statement-boundary test's outcome: .next is past a terminator, or a brace's own position
// (braces are left in place for parse_block / the scope's owner to read).
typedef struct separator_result {
    bool     ends;
    uint32_t next;
} separator_result;

// Whether the statement ends here: a terminator, a scope-closing '}' or a scope-opening '{'.
// Non-erroring, for the speculative callers (opcode no-operand peeks, macro overload matching).
separator_result peek_separator(baron *b, cursor at);

// The erroring form: a missing boundary is a fatal syntax error.
parse_result require_separator(baron *b, cursor at);

// Evaluate one expression in the assembler's current context - the single place that projects
// baron into an expr_env.
expr_result eval(baron *b, cursor at, uint32_t scope, uint32_t section, rc_arena scratch);

// How a non-emitting statement sits in the listing.
typedef enum verbose_text_kind {
    verbose_text_margin = 0,   // at the margin: labels, braces, assignments, SECTION framing
    verbose_text_address,      // address + empty byte field: macro invocations, INCLUDE
} verbose_text_kind;

// Append one listing line, only when flags.listing && flags.active: a code line is address + hex
// dump + source echo (sliced [stmt.pos, end_pos), first line only); a text line is the rest.
void verbose_code_line(baron *b, parse_flags flags, cursor stmt, uint32_t end_pos,
                       uint32_t section, uint32_t pc, uint32_t code_begin);
void verbose_text_line(baron *b, parse_flags flags, cursor stmt, uint32_t end_pos,
                       uint32_t pc, verbose_text_kind kind);


#endif // ifndef BARON_ASSEMBLE_INTERNAL_H_
