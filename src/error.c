#include "error.h"

#include "richc/macros.h"   // RC_UNREACHABLE


// A short human-readable message for a code - capitalised, no jargon underscores, informative enough to
// stand on its own next to a source location. (A later pass may append specifics, e.g. the symbol name.)
rc_str error_type_name(error_type e)
{
    switch (e) {
        case error_type_none:                    return RC_STR("No error");
        case error_type_unexpected_token:        return RC_STR("Unexpected token");
        case error_type_unexpected_close_brace:  return RC_STR("Unexpected '}'");
        case error_type_unclosed_scope:          return RC_STR("Unclosed '{': expected a matching '}'");
        case error_type_expected_separator:      return RC_STR("Expected a newline or ':' after this statement");
        case error_type_expected_label_name:     return RC_STR("Expected a label name after '.'");
        case error_type_expected_overlay_name:   return RC_STR("OVERLAY expects an overlay name");
        case error_type_invalid_assignment:      return RC_STR("Cannot assign to a dotted name");
        case error_type_expected_assign:         return RC_STR("Expected '=' in assignment");
        case error_type_expected_close_paren:    return RC_STR("Expected ')'");
        case error_type_bad_index_register:      return RC_STR("Expected index register X or Y");
        case error_type_missing_operand:         return RC_STR("This instruction needs an operand");
        case error_type_expression:              return RC_STR("Malformed expression");
        case error_type_unclosed_if:             return RC_STR("Unclosed IF: expected ENDIF");
        case error_type_unexpected_elif:         return RC_STR("ELIF without a matching IF");
        case error_type_unexpected_else:         return RC_STR("ELSE without a matching IF");
        case error_type_unexpected_endif:        return RC_STR("ENDIF without a matching IF");
        case error_type_unclosed_for:            return RC_STR("Unclosed FOR: expected NEXT");
        case error_type_unexpected_next:         return RC_STR("NEXT without a matching FOR");
        case error_type_reserved_constant:       return RC_STR("TRUE, FALSE and PI are built-in constants and cannot be redefined");
        case error_type_expected_macro_name:     return RC_STR("Expected a name after MACRO");
        case error_type_macro_name_reserved:     return RC_STR("A macro cannot be named after a mnemonic, keyword or constant");
        case error_type_unquoted_macro_token:    return RC_STR("A literal token in a macro signature must be quoted");
        case error_type_unclosed_macro:          return RC_STR("Unclosed MACRO: expected ENDMACRO");
        case error_type_unexpected_endmacro:     return RC_STR("ENDMACRO without a matching MACRO");
        case error_type_expected_function_name:  return RC_STR("Expected a name after FUNCTION");
        case error_type_function_name_reserved:  return RC_STR("A function cannot be named after a mnemonic, keyword or constant");
        case error_type_expected_function_params:return RC_STR("Expected a parenthesised parameter list");
        case error_type_unclosed_function:       return RC_STR("Unclosed FUNCTION: expected an '=' return");
        case error_type_bad_addressing_mode:     return RC_STR("This instruction has no such addressing mode");
        case error_type_operand_not_numeric:     return RC_STR("Operand is not a number");
        case error_type_value_out_of_range:      return RC_STR("Value out of range");
        case error_type_branch_out_of_range:     return RC_STR("Branch target is out of range");
        case error_type_skip_backwards:          return RC_STR("Cannot move the program counter backwards");
        case error_type_bad_alignment:           return RC_STR("Alignment must be at least 1");
        case error_type_undefined_symbol:        return RC_STR("Undefined symbol");
        case error_type_duplicate_symbol:        return RC_STR("Symbol is already defined in this scope");
        case error_type_original_definition:     return RC_STR("Originally defined here");
        case error_type_not_iterable:            return RC_STR("FOR needs a list or a range to iterate over");
        case error_type_expected_filename:       return RC_STR("INCLUDE expects a filename string");
        case error_type_include_too_deep:        return RC_STR("INCLUDE nested too deeply (a cyclic include?)");
        case error_type_included_from:           return RC_STR("Included from here");
        case error_type_duplicate_signature:     return RC_STR("This macro signature is already defined");
        case error_type_no_matching_signature:   return RC_STR("No macro overload matches this invocation");
        case error_type_macro_not_defined:       return RC_STR("Macro is only forward-declared: its body is not defined here");
        case error_type_macro_too_deep:          return RC_STR("Macro expansion nested too deeply (a missing recursion base case?)");
        case error_type_expanded_from:           return RC_STR("Expanded from here");
        case error_type_no_matching_arity:       return RC_STR("No function overload takes this number of arguments");
        case error_type_duplicate_function:      return RC_STR("This function arity is already defined");
        case error_type_function_not_defined:    return RC_STR("Function is only forward-declared: its body is not defined here");
        case error_type_function_too_deep:       return RC_STR("Function recursion nested too deeply (a missing recursion base case?)");
        case error_type_divide_by_zero:          return RC_STR("Division by zero");
        case error_type_domain:                  return RC_STR("Argument outside the valid range for this operation");
        case error_type_unknown_symbol:          return RC_STR("Unknown symbol");
        case error_type_type_mismatch:           return RC_STR("Operands have incompatible types");
        case error_type_subscript_range:         return RC_STR("Subscript out of range");
        case error_type_incorrect_parameters:    return RC_STR("Wrong number or kind of arguments");
        case error_type_shape_mismatch:          return RC_STR("Operand shapes do not match");
        case error_type_list_too_big:            return RC_STR("List is too large");
        case error_type_not_implemented:         return RC_STR("Not implemented yet");
        case error_type_no_convergence:          return RC_STR("Assembly did not settle (a size keeps changing)");
        case error_type_source_load:             return RC_STR("Could not read the source file");
        case error_type_jmp_indirect_page_cross: return RC_STR("Indirect JMP vector straddles a page boundary (6502 hardware bug)");
    }
    RC_UNREACHABLE();
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(error, names)
{
    // Every code has a message (the switch is exhaustive, so a missing one would not compile), and it is
    // the human-readable form, not the enumerator tail.
    RC_CHECK(error_type_name(error_type_none),           ==, RC_STR("No error"));
    RC_CHECK(error_type_name(error_type_divide_by_zero), ==, RC_STR("Division by zero"));
    RC_CHECK(error_type_name(error_type_duplicate_symbol), ==, RC_STR("Symbol is already defined in this scope"));
}

#endif // BARON_TESTS
