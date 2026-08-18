#include "error.h"

#include "richc/macros.h"   // RC_UNREACHABLE
#include "richc/mstr.h"


// The terse message template for a code - capitalised, no jargon underscores, short enough to scan in a
// column of diagnostics. A '%' marks where a diagnostic's payload (a symbol name, a branch distance, an
// ERROR statement's text) is substituted at render time (see error_append_message below).
rc_str error_type_name(error_type e)
{
    switch (e) {
        case error_type_none:                    return RC_STR("No error");
        case error_type_unexpected_token:        return RC_STR("Unexpected token");
        case error_type_unexpected_close_brace:  return RC_STR("Unexpected '}'");
        case error_type_unclosed_scope:          return RC_STR("Unclosed '{'");
        case error_type_expected_separator:      return RC_STR("Expected a newline or ':'");
        case error_type_expected_label_name:     return RC_STR("Expected a label name");
        case error_type_expected_section_name:   return RC_STR("Expected a section name");
        case error_type_unclosed_section:        return RC_STR("Unclosed SECTION");
        case error_type_unexpected_endsection:   return RC_STR("ENDSECTION without a SECTION");
        case error_type_expected_var_name:       return RC_STR("Expected a variable name");
        case error_type_invalid_assignment:      return RC_STR("Cannot assign to a dotted name");
        case error_type_expected_assign:         return RC_STR("Expected '='");
        case error_type_expected_close_paren:    return RC_STR("Expected ')'");
        case error_type_bad_index_register:      return RC_STR("Expected index register X or Y");
        case error_type_missing_operand:         return RC_STR("Missing operand");
        case error_type_expression:              return RC_STR("Malformed expression");
        case error_type_unclosed_if:             return RC_STR("Unclosed IF");
        case error_type_unexpected_elif:         return RC_STR("ELIF without an IF");
        case error_type_unexpected_else:         return RC_STR("ELSE without an IF");
        case error_type_unexpected_endif:        return RC_STR("ENDIF without an IF");
        case error_type_unclosed_for:            return RC_STR("Unclosed FOR");
        case error_type_unexpected_next:         return RC_STR("NEXT without a FOR");
        case error_type_reserved_constant:       return RC_STR("Cannot redefine a built-in constant");
        case error_type_expected_macro_name:     return RC_STR("Expected a macro name");
        case error_type_macro_name_reserved:     return RC_STR("Reserved macro name");
        case error_type_unquoted_macro_token:    return RC_STR("Unquoted literal token in macro signature");
        case error_type_unclosed_macro:          return RC_STR("Unclosed MACRO");
        case error_type_unexpected_endmacro:     return RC_STR("ENDMACRO without a MACRO");
        case error_type_expected_function_name:  return RC_STR("Expected a function name");
        case error_type_function_name_reserved:  return RC_STR("Reserved function name");
        case error_type_expected_function_params:return RC_STR("Expected a parameter list");
        case error_type_unclosed_function:       return RC_STR("Unclosed FUNCTION");
        case error_type_bad_print_channel:       return RC_STR("Bad PRINT channel (expected #0-#9 and a comma)");
        case error_type_unclosed_basic:          return RC_STR("Unclosed BASIC");
        case error_type_unexpected_endbasic:     return RC_STR("ENDBASIC without a BASIC");
        case error_type_expected_basic_line:     return RC_STR("Expected a BASIC line or ENDBASIC");
        case error_type_bad_addressing_mode:     return RC_STR("No such addressing mode");
        case error_type_needs_cmos:              return RC_STR("CMOS-only instruction (needs cmos=TRUE on the section)");
        case error_type_operand_not_numeric:     return RC_STR("Operand is not a number");
        case error_type_value_out_of_range:      return RC_STR("Value out of range");
        case error_type_branch_out_of_range:     return RC_STR("Branch out of range: % bytes");
        case error_type_skip_backwards:          return RC_STR("Cannot move the program counter backwards");
        case error_type_bad_alignment:           return RC_STR("Alignment must be at least 1");
        case error_type_bad_basic_line_number:   return RC_STR("BASIC line number must be 0 to 32767");
        case error_type_basic_line_too_long:     return RC_STR("BASIC line longer than 255 bytes");
        case error_type_reserve_not_zeropage:    return RC_STR("ZPRESERVE address outside the zero page");
        case error_type_var_without_reserve:     return RC_STR("ZPAUTO needs a prior ZPRESERVE");
        case error_type_zeropage_full:           return RC_STR("No free zero-page byte for ZPAUTO variable: '%'");
        case error_type_zpauto_across_call:      return RC_STR("ZPAUTO variable live across an unanalysable JSR (annotate with CANCALL)");
        case error_type_zpauto_recursion:        return RC_STR("ZPAUTO variable freshly written and held live across recursion");
        case error_type_zpauto_computed_flow:    return RC_STR("Computed jump reaches unknown code (annotate with CANJUMP)");
        case error_type_zpauto_register_name:    return RC_STR("ZPAUTO variable cannot be named 'A'");
        case error_type_zpauto_indexed_access:   return RC_STR("Unchecked indexed access into ZPAUTO variable: '%'");
        case error_type_zpauto_unused:           return RC_STR("Unused ZPAUTO variable: '%'");
        case error_type_zpauto_narrow_pointer:   return RC_STR("ZPAUTO1 dereferenced as a pointer (declare it ZPAUTO2)");
        case error_type_zpauto_out_of_bounds:    return RC_STR("Access past the end of ZPAUTO variable");
        case error_type_zpauto_bad_width:        return RC_STR("ZPAUTO count must be 1 to 256");
        case error_type_zpauto_address:          return RC_STR("Cannot use a ZPAUTO address here: '%'");
        case error_type_undefined_symbol:        return RC_STR("Undefined symbol: '%'");
        case error_type_duplicate_symbol:        return RC_STR("Duplicate symbol: '%'");
        case error_type_original_definition:     return RC_STR("First defined here: '%'");
        case error_type_duplicate_section:       return RC_STR("Duplicate section: '%'");
        case error_type_unknown_section:         return RC_STR("No section named: '%'");
        case error_type_circular_incsection:     return RC_STR("Circular INCSECTION: '%'");
        case error_type_not_iterable:            return RC_STR("FOR needs a list or a range");
        case error_type_expected_filename:       return RC_STR("Expected a filename string");
        case error_type_include_too_deep:        return RC_STR("INCLUDE nested too deeply");
        case error_type_included_from:           return RC_STR("Included from here");
        case error_type_duplicate_signature:     return RC_STR("Duplicate macro signature");
        case error_type_no_matching_signature:   return RC_STR("No matching macro overload");
        case error_type_macro_not_defined:       return RC_STR("Macro body is not defined");
        case error_type_macro_too_deep:          return RC_STR("Macro expansion nested too deeply");
        case error_type_expanded_from:           return RC_STR("Expanded from here");
        case error_type_no_matching_arity:       return RC_STR("No matching function overload");
        case error_type_duplicate_function:      return RC_STR("Duplicate function arity");
        case error_type_function_not_defined:    return RC_STR("Function body is not defined");
        case error_type_function_too_deep:       return RC_STR("Function recursion nested too deeply");
        case error_type_divide_by_zero:          return RC_STR("Division by zero");
        case error_type_domain:                  return RC_STR("Argument out of domain");
        case error_type_unknown_symbol:          return RC_STR("Unknown symbol: '%'");
        case error_type_type_mismatch:           return RC_STR("Incompatible types");
        case error_type_subscript_range:         return RC_STR("Subscript out of range");
        case error_type_incorrect_parameters:    return RC_STR("Wrong number or kind of arguments");
        case error_type_shape_mismatch:          return RC_STR("Operand shapes do not match");
        case error_type_list_too_big:            return RC_STR("List too large");
        case error_type_not_implemented:         return RC_STR("Not implemented");
        case error_type_user_error:              return RC_STR("%");
        case error_type_no_convergence:          return RC_STR("Assembly did not settle");
        case error_type_source_load:             return RC_STR("Could not read the source file");
        case error_type_jmp_indirect_page_cross: return RC_STR("Indirect JMP vector straddles a page boundary (6502 bug)");
    }
    RC_UNREACHABLE();
}

void error_append_message(rc_mstr *out, error_type e, rc_str payload, rc_arena *arena)
{
    rc_str msg = error_type_name(e);
    uint32_t pct = rc_str_find_first(msg, RC_STR("%"));
    if (pct != RC_INDEX_NONE) {
        rc_mstr_append(out, rc_str_left(msg, pct), arena);
        if (payload.len != 0) {
            rc_mstr_append(out, payload, arena);
        }
        rc_mstr_append(out, rc_str_skip(msg, pct + 1), arena);
    }
    else {
        rc_mstr_append(out, msg, arena);   // no '%': the message stands alone, any payload is dropped
    }
}


#ifdef BARON_TESTS

#include "richc/test.h"
#include "richc/arena.h"

RC_TEST(error, names)
{
    // Every code has a message (the switch is exhaustive, so a missing one would not compile), and it is
    // the terse human-readable form, not the enumerator tail.
    RC_CHECK(error_type_name(error_type_none),             ==, RC_STR("No error"));
    RC_CHECK(error_type_name(error_type_divide_by_zero),   ==, RC_STR("Division by zero"));
    RC_CHECK(error_type_name(error_type_duplicate_symbol), ==, RC_STR("Duplicate symbol: '%'"));
}

RC_TEST(error, message_substitution)
{
    rc_arena arena = rc_arena_make_default();

    // A payload lands on the template's first '%' (an empty one substitutes nothing); a payload
    // containing '%' is inert (only the template is scanned); no '%' in the template drops the payload.
    rc_mstr m = rc_mstr_make(16, &arena);
    error_append_message(&m, error_type_undefined_symbol, RC_STR("zork"), &arena);
    RC_CHECK(m.view, ==, RC_STR("Undefined symbol: 'zork'"));

    m = rc_mstr_make(16, &arena);
    error_append_message(&m, error_type_user_error, RC_STR("50% done"), &arena);
    RC_CHECK(m.view, ==, RC_STR("50% done"));

    m = rc_mstr_make(16, &arena);
    error_append_message(&m, error_type_undefined_symbol, (rc_str) {0}, &arena);
    RC_CHECK(m.view, ==, RC_STR("Undefined symbol: ''"));

    m = rc_mstr_make(16, &arena);
    error_append_message(&m, error_type_divide_by_zero, RC_STR("ignored"), &arena);
    RC_CHECK(m.view, ==, RC_STR("Division by zero"));

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
