#include "error.h"

#include "richc/macros.h"   // RC_UNREACHABLE


rc_str error_type_name(error_type e)
{
    switch (e) {
        case error_type_none:                    return RC_STR("none");
        case error_type_unexpected_token:        return RC_STR("unexpected_token");
        case error_type_unexpected_close_brace:  return RC_STR("unexpected_close_brace");
        case error_type_unclosed_scope:          return RC_STR("unclosed_scope");
        case error_type_expected_separator:      return RC_STR("expected_separator");
        case error_type_expected_label_name:     return RC_STR("expected_label_name");
        case error_type_invalid_assignment:      return RC_STR("invalid_assignment");
        case error_type_expected_assign:         return RC_STR("expected_assign");
        case error_type_expected_close_paren:    return RC_STR("expected_close_paren");
        case error_type_bad_index_register:      return RC_STR("bad_index_register");
        case error_type_missing_operand:         return RC_STR("missing_operand");
        case error_type_expression:              return RC_STR("expression");
        case error_type_unclosed_if:             return RC_STR("unclosed_if");
        case error_type_unexpected_elif:         return RC_STR("unexpected_elif");
        case error_type_unexpected_else:         return RC_STR("unexpected_else");
        case error_type_unexpected_endif:        return RC_STR("unexpected_endif");
        case error_type_unclosed_for:            return RC_STR("unclosed_for");
        case error_type_unexpected_next:         return RC_STR("unexpected_next");
        case error_type_bad_addressing_mode:     return RC_STR("bad_addressing_mode");
        case error_type_operand_not_numeric:     return RC_STR("operand_not_numeric");
        case error_type_value_out_of_range:      return RC_STR("value_out_of_range");
        case error_type_branch_out_of_range:     return RC_STR("branch_out_of_range");
        case error_type_skip_backwards:          return RC_STR("skip_backwards");
        case error_type_bad_alignment:           return RC_STR("bad_alignment");
        case error_type_undefined_symbol:        return RC_STR("undefined_symbol");
        case error_type_duplicate_symbol:        return RC_STR("duplicate_symbol");
        case error_type_not_iterable:            return RC_STR("not_iterable");
        case error_type_divide_by_zero:          return RC_STR("divide_by_zero");
        case error_type_domain:                  return RC_STR("domain");
        case error_type_unknown_symbol:          return RC_STR("unknown_symbol");
        case error_type_type_mismatch:           return RC_STR("type_mismatch");
        case error_type_subscript_range:         return RC_STR("subscript_range");
        case error_type_incorrect_parameters:    return RC_STR("incorrect_parameters");
        case error_type_shape_mismatch:          return RC_STR("shape_mismatch");
        case error_type_list_too_big:            return RC_STR("list_too_big");
        case error_type_not_implemented:         return RC_STR("not_implemented");
        case error_type_no_convergence:          return RC_STR("no_convergence");
        case error_type_source_load:             return RC_STR("source_load");
        case error_type_jmp_indirect_page_cross: return RC_STR("jmp_indirect_page_cross");
    }
    RC_UNREACHABLE();
}
