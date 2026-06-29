#ifndef BARON_ASSEMBLE_H_
#define BARON_ASSEMBLE_H_

#include "richc/bytes.h"   // rc_view_bytes, rc_str
#include "richc/arena.h"   // rc_arena (taken by value)


// The assembler: parse 6502 source, lay down object code in the default overlay, define labels
// and symbols, and run passes until everything settles. The managers it works through (scopes,
// overlays, source files) live in `baron`, which the entry points need only by pointer.
typedef struct baron baron;

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
    assemble_error_skip_backwards,         // SKIP/SKIPTO would move the pointer backwards
    assemble_error_bad_alignment,          // ALIGN n with n < 1
    assemble_error_undefined_symbol,
    assemble_error_duplicate_symbol,       // a name defined twice in one scope
    assemble_error_unclosed_if,            // IF reached '}' or end of input before ENDIF
    assemble_error_unexpected_endif,       // ELIF / ELSE / ENDIF with no IF to match
    assemble_error_unclosed_for,           // FOR reached '}' or end of input before NEXT
    assemble_error_unexpected_next,        // NEXT with no FOR to match
    assemble_error_not_iterable,           // a FOR sequence that is neither a list nor a range
    assemble_error_expression,             // a parse error inside an operand expression
    assemble_error_no_convergence,
    assemble_error_source_load,            // a source file could not be read
} assemble_error;

typedef struct assemble_result {
    rc_view_bytes  code;      // the assembled object code (valid only when error == none)
    uint32_t       passes;    // how many passes it took
    assemble_error error;
    uint32_t       error_at;  // source offset of the error (when error != none)
} assemble_result;

// Assemble a source into b's default overlay, running passes until labels and forward references
// settle (or reporting non-convergence). Each entry point caches its source as a source file in
// b (the text under a name, or a loaded file) and then runs the shared pass loop from that source
// index. scratch is a working arena taken by value, so the caller's copy is left untouched. The
// returned code view points into b's object-code arena and stays valid until the next assemble.
assemble_result assemble_string(baron *b, rc_str name, rc_str text, rc_arena scratch);
assemble_result assemble_file(baron *b, rc_str path, rc_arena scratch);

rc_str assemble_error_name(assemble_error e);


#endif // ifndef BARON_ASSEMBLE_H_
