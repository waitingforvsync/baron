#ifndef BARON_ERROR_H_
#define BARON_ERROR_H_

#include "richc/mstr.h"
#include "richc/str.h"


// The single diagnostic vocabulary, shared by every layer: the expression evaluator emits the
// value-evaluation codes straight into a value, the assembler the rest, so a value's error surfaces
// as a diagnostic with no translation step. (Expression PARSE errors keep their own expr_error enum,
// wrapped as error_type_expression.) error_type_unknown_symbol is the transient "not bound yet" that
// may still resolve on a later pass; judged final, it becomes error_type_undefined_symbol instead.
typedef enum error_type {
    error_type_none,

    // Lexical / structural: the statement stream is malformed (a fatal parse).
    error_type_unexpected_token,
    error_type_unexpected_close_brace,
    error_type_unclosed_scope,
    error_type_expected_separator,
    error_type_expected_label_name,
    error_type_expected_section_name, // SECTION was not followed by a name
    error_type_unclosed_section,      // SECTION reached '}' / a foreign closer / end of input before ENDSECTION
    error_type_unexpected_endsection, // ENDSECTION with no SECTION to match
    error_type_expected_var_name,     // ZA_AUTO1/ZA_AUTO2 was not followed by a (bare) name
    error_type_invalid_assignment,    // a dotted path on the left of '='
    error_type_expected_assign,       // a bare identifier statement with no '='
    error_type_expected_close_paren,
    error_type_bad_index_register,    // expected X or Y after a ','
    error_type_missing_operand,
    error_type_expression,            // a parse error inside an operand expression
    error_type_expected_end_of_expression, // text remains after an expression that must stand alone (a -D define)
    error_type_unclosed_if,           // IF reached '}' or end of input before ENDIF
    error_type_unexpected_elif,       // ELIF with no IF to match
    error_type_unexpected_else,       // ELSE with no IF to match
    error_type_unexpected_endif,      // ENDIF with no IF to match
    error_type_unclosed_for,          // FOR reached '}' or end of input before NEXT
    error_type_unexpected_next,       // NEXT with no FOR to match
    error_type_reserved_constant,     // a built-in constant (TRUE/FALSE/PI) used as a statement / assignment target
    error_type_expected_macro_name,   // MACRO with no name after it
    error_type_macro_name_reserved,   // a macro named like a mnemonic / keyword / constant, or a dotted name
    error_type_unquoted_macro_token,  // a bare (non-comma) literal token in a signature - it must be quoted
    error_type_unclosed_macro,        // MACRO reached a foreign closer or end of input before ENDMACRO
    error_type_unexpected_endmacro,   // ENDMACRO with no MACRO to match
    error_type_expected_function_name,  // FUNCTION with no name after it
    error_type_function_name_reserved,  // a function named like a mnemonic / keyword / constant, or a dotted name
    error_type_expected_function_params,// a malformed parameter list (no '(', or a non-identifier parameter)
    error_type_unclosed_function,       // a FUNCTION body reached EOF / a foreign closer with no '=' return
    error_type_bad_print_channel,       // PRINT's '#' not followed by a single digit 0-9 and a comma
    error_type_unclosed_basic,          // BASIC reached a foreign closer or end of input before ENDBASIC
    error_type_unexpected_endbasic,     // ENDBASIC with no BASIC to match
    error_type_expected_basic_line,     // a statement inside BASIC that is neither a numbered line nor ENDBASIC

    // Semantic: the statement parses, but its meaning is wrong.
    error_type_bad_addressing_mode,   // the mnemonic has no encoding for that operand shape
    error_type_needs_cmos,            // the encoding exists, but only on the 65C02: set cmos=TRUE on the section
    error_type_operand_not_numeric,   // an operand evaluated to a string / list where a number was needed
    error_type_value_out_of_range,
    error_type_branch_out_of_range,
    error_type_skip_backwards,        // SKIP / SKIPTO would move the pointer backwards
    error_type_bad_alignment,         // ALIGN n with n < 1
    error_type_bad_basic_line_number, // a BASIC line number past 32767 (the ROM's enterable maximum)
    error_type_basic_line_too_long,   // a tokenised BASIC line record past 255 bytes (its length is one byte)
    error_type_guard_exceeded,        // a section's emission ran past its guard address
    error_type_reserve_not_zeropage,  // a ZA_POOL address falls outside the zero page ($00-$FF)
    error_type_var_without_reserve,   // a ZA_AUTO1/ZA_AUTO2 declared with no ZA_POOL enabling the feature first
    error_type_zeropage_full,         // no free reserved byte to place a ZA_AUTO variable (a spill)
    error_type_za_auto_across_call,   // a var live across a JSR whose callee footprint cannot be bounded
    error_type_za_auto_recursion,     // a var FRESHLY written then held live across a recursive call (needs a byte per level)
    error_type_za_auto_computed_flow, // ZA_AUTO active past a computed/indirect jump the analysis cannot follow
    error_type_za_auto_register_name, // a ZA_AUTO variable named 'A' - ambiguous with accumulator addressing
    error_type_za_auto_indexed_access, // a ZA_AUTO reached by indexed / indexed-indirect addressing (var,X etc.) - opt-in warning, user owns the index bounds
    error_type_za_auto_unused,        // a ZA_AUTO no instruction touches - warned, given no address, and left undefined
    error_type_za_auto_narrow_pointer, // a 1-byte ZA_AUTO1 dereferenced as a pointer ((var),Y / (var)) - needs ZA_AUTO2
    error_type_za_auto_out_of_bounds, // a var+n access reaches past the ZA_AUTO variable's declared width
    error_type_za_auto_bad_width,     // ZA_AUTO <count> with a count outside 1..256
    error_type_za_auto_address,       // a ZA_AUTO address used where a number is needed NOW - addresses exist only after allocation
    error_type_za_discard_needs_var,  // a ZA_DISCARD operand that is not a whole ZA_AUTO variable
    error_type_za_entry_no_code,      // a ZA_ENTRY/ZA_INTERRUPT marker whose pc begins no assembled instruction
    error_type_za_entry_input,        // warning: a ZA_ENTRY routine reads a ZA_AUTO before writing it - callers cannot know its address
    error_type_za_auto_unreachable,   // warning: ZA_AUTO-touching code unreachable from every declared/default root
    error_type_za_returnto_no_code,   // warning: a ZA_RETURNTO target that begins no assembled instruction
    error_type_skip_spans_instruction, // a BITZP/BITABS resume address landing inside a recorded instruction
    error_type_undefined_symbol,      // a reference still unresolved on the final pass
    error_type_duplicate_symbol,      // a name defined twice in one scope
    error_type_original_definition,   // the companion to duplicate_symbol: points at the first binding
    error_type_duplicate_section,     // two SECTION blocks share a name (names are unique; no concatenation)
    error_type_unknown_section,       // INCSECTION names a section that never appeared
    error_type_circular_incsection,   // INCSECTION dependencies form a cycle (self-insertion included)
    error_type_not_iterable,          // a FOR sequence that is neither a list nor a range
    error_type_expected_filename,     // an INCLUDE / INCBIN operand did not evaluate to a string
    error_type_include_too_deep,      // INCLUDE recursion hit the depth cap - a cyclic include, most likely
    error_type_included_from,         // the companion frame: points at the INCLUDE that pulled in a failing file
    error_type_duplicate_signature,   // a second body defined for a macro signature already given one
    error_type_no_matching_signature, // a macro call whose arguments fit none of its overloads
    error_type_macro_not_defined,     // a macro invoked while only forward-declared (its body never supplied here)
    error_type_macro_too_deep,        // macro expansion hit the depth cap - a missing/wrong recursion base case
    error_type_expanded_from,         // the companion frame: points at the macro invocation an error expanded from
    error_type_no_matching_arity,     // a function call whose argument count fits none of its overloads
    error_type_duplicate_function,    // a second body defined for a function arity already given one
    error_type_function_not_defined,  // a function invoked while only forward-declared (its body never supplied)
    error_type_function_too_deep,     // function recursion hit the depth cap - a missing/wrong recursion base case

    // Value-evaluation errors: produced by the expression evaluator, carried inside a value.
    error_type_divide_by_zero,
    error_type_domain,
    error_type_unknown_symbol,        // not bound yet: a forward reference, may resolve on a later pass
    error_type_type_mismatch,
    error_type_subscript_range,
    error_type_incorrect_parameters,
    error_type_shape_mismatch,        // a ragged operand, or shapes that do not broadcast
    error_type_list_too_big,          // a list grew past VALUE_LIST_MAX_LENGTH
    error_type_not_found,             // find() scanned the whole haystack and the needle was not in it
    error_type_not_implemented,

    // The ERROR statement: the user's own message, carried whole in the diagnostic's payload.
    error_type_user_error,

    // Driver.
    error_type_no_convergence,
    error_type_unsettled_symbol,      // no_convergence with a culprit: the first binding that kept moving
    error_type_source_load,           // a source file could not be read

    // Warnings (recorded with diagnostic_warning; harmless, they do not fail the assemble).
    error_type_jmp_indirect_page_cross,   // JMP (&xxFF): the NMOS vector-fetch page-wrap bug
} error_type;

// The terse human-readable message template for a code (not the enumerator tail) - what a rendered
// diagnostic prints after its location. A '%' in the template marks where a diagnostic's payload (a
// symbol name, a branch distance, an ERROR statement's text) belongs.
rc_str error_type_name(error_type e);

// Append the code's message to out with the payload substituted for the template's first '%' (an empty
// payload substitutes nothing). Only the template is scanned, so a payload containing '%' is inert; a
// payload with no '%' to land in is dropped. The one message renderer, shared by the diagnostic report
// and value_format.
void error_append_message(rc_mstr *out, error_type e, rc_str payload, rc_arena *arena);


#endif // ifndef BARON_ERROR_H_
