#ifndef BARON_VALUE_H_
#define BARON_VALUE_H_

#include "richc/arena.h"
#include "richc/mstr.h"
#include "richc/str.h"


// The single value type flowing through Baron's expression machinery. It is a
// tagged union: none (uninitialised), a number, a string, a list of values, a
// numeric range, or an error. Evaluation errors (unknown symbol, divide by zero,
// ...) are carried as a value rather than aborting, so they can propagate to the
// end of an expression and let assembly continue across an as-yet-unresolvable
// reference.

// value is recursive (a list holds values), so its view/span/array types are
// declared here while value is only forward-declared, and the matching functions
// are emitted once value is complete (see the second include below).
typedef struct value value;

#define RC_ARRAY_TYPE value
#define RC_ARRAY_NAME  value
#define RC_ARRAY_DECLARE_ONLY
#include "richc/template/array.h"

// A list value is exactly a read-only view of values.
typedef rc_view_value value_list;


typedef enum value_error {
    value_error_none,                   // zero/default: no error
    value_error_divide_by_zero,
    value_error_domain,
    value_error_unknown_symbol,
    value_error_type_mismatch,
    value_error_subscript_range,
    value_error_incorrect_parameters,
    value_error_not_implemented,
} value_error;


typedef enum value_type {
    value_type_none,                    // zero/default: uninitialised
    value_type_numeric,
    value_type_string,
    value_type_list,
    value_type_range,
    value_type_error,
} value_type;


// An inclusive numeric range. start/end are optional so boundless ranges (3..,
// ..9, ..) can be represented; step defaults to 1. There is no exclusivity flag:
// an exclusive end (0..<10) is normalised to an inclusive end at construction, so
// iteration is uniformly `v <= end`.
typedef struct value_range {
    int64_t start;
    int64_t end;
    int64_t step;
    bool    has_start;
    bool    has_end;
} value_range;


struct value {
    value_type type;
    union {
        double      numeric;
        rc_str      string;             // view into source or arena; concat allocates
        value_list  list;               // rc_view_value: const value *, num
        value_range range;
        value_error error;
    };
};

// value is now complete: emit the view/span/array functions.
#define RC_ARRAY_TYPE value
#define RC_ARRAY_NAME  value
#define RC_ARRAY_IMPL_ONLY
#include "richc/template/array.h"


// Constructors (by value).
value value_make_none(void);
value value_make_numeric(double n);
value value_make_string(rc_str s);
value value_make_error(value_error e);
value value_make_range(value_range r);
value value_make_list(rc_view_value items);    // wraps the view; does not copy

// Range builders. They take the (already evaluated) endpoint values and own all the
// range semantics: step inference, the second-element/stepped form (rhs is a range),
// exclusivity, and validation. Each returns a range value, or a propagating error
// value (a bad endpoint, a non-integer, an empty or inconsistent range).
value value_make_range_pair(value lhs, value rhs, bool exclusive);   // lhs..rhs / lhs..<rhs
value value_make_range_open_end(value lhs, bool exclusive);          // lhs..
value value_make_range_open_start(value rhs, bool exclusive);        // ..rhs / ..<rhs
value value_make_range_open(bool exclusive);                         // ..

// Type queries.
value_type value_type_of(value v);
bool value_is_none(value v);
bool value_is_numeric(value v);
bool value_is_string(value v);
bool value_is_list(value v);
bool value_is_range(value v);
bool value_is_error(value v);

// Structural equality: types must match, then compared field-wise (lists
// element-wise, recursively).
bool value_is_equal(value a, value b);

// Diagnostics.
rc_str value_error_name(value_error e);
void value_format(rc_mstr *out, value v, rc_arena *arena);


#endif // ifndef BARON_VALUE_H_
