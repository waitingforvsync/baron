#ifndef BARON_ASSEMBLE_H_
#define BARON_ASSEMBLE_H_

#include "scopes.h"
#include "overlay.h"
#include "richc/bytes.h"


// The assembler: parse 6502 source from an rc_str, lay down object code in the default
// overlay, define labels and symbols, and run passes until everything settles. No file
// loading yet - INCLUDE brings that later.

typedef enum assemble_error {
    assemble_error_none,
    assemble_error_unexpected_token,
    assemble_error_unexpected_close_brace,
    assemble_error_unclosed_scope,
    assemble_error_expected_separator,
    assemble_error_expected_label_name,
    assemble_error_invalid_assignment,    // a dotted path on the left of '='
    assemble_error_expected_assign,       // a bare identifier statement with no '='
    assemble_error_expected_close_paren,
    assemble_error_bad_index_register,     // expected X or Y after a ','
    assemble_error_missing_operand,
    assemble_error_bad_addressing_mode,    // the mnemonic has no encoding for that operand shape
    assemble_error_operand_not_numeric,    // an operand evaluated to a string/list/real error
    assemble_error_value_out_of_range,
    assemble_error_branch_out_of_range,
    assemble_error_undefined_symbol,
    assemble_error_expression,             // a parse error inside an operand expression
    assemble_error_no_convergence,
} assemble_error;

typedef struct assemble_result {
    rc_view_bytes  code;      // the assembled object code (valid only when error == none)
    uint32_t       passes;    // how many passes it took
    assemble_error error;
    uint32_t       error_at;  // source offset of the error (when error != none)
} assemble_result;

// Assemble source into the overlay o, resolving symbols against the scopes s, running passes
// until labels and forward references settle (or reporting non-convergence). scratch is a
// working arena, taken by value: assemble leaves the caller's copy untouched. The returned code
// view points into o's object-code arena and stays valid until the next assemble into o.
assemble_result assemble(scopes *s, overlay *o, rc_str source, rc_arena scratch);

rc_str assemble_error_name(assemble_error e);


#endif // ifndef BARON_ASSEMBLE_H_
