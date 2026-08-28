#ifndef BARON_VALUE_H_
#define BARON_VALUE_H_

#include "richc/arena.h"
#include "richc/mstr.h"
#include "richc/str.h"
#include "cursor.h"  // cursor, half of a ZPAUTO variable's identity
#include "error.h"   // error_type, the shared diagnostic vocabulary


// The single value type flowing through Baron's expression machinery. It is a
// tagged union: none (uninitialised), a number, a boolean, a string, a list of
// values, a numeric range, an error, or a ZPAUTO address. Evaluation errors (unknown
// symbol, divide by zero, ...) are carried as a value rather than aborting, so
// they can propagate to the end of an expression and let assembly continue
// across an as-yet-unresolvable reference.

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

// The most elements a list may be materialised to. Enumerating a huge range (say by subscripting it)
// would otherwise build an unbounded list; past this it is a error_type_list_too_big instead.
#define VALUE_LIST_MAX_LENGTH 65536u




typedef enum value_type {
    value_type_none,                    // zero/default: uninitialised
    value_type_numeric,
    value_type_boolean,                 // TRUE / FALSE; coerces to 1 / 0 in any numeric context
    value_type_string,
    value_type_list,
    value_type_range,
    value_type_error,
    value_type_zpauto,                  // the address of a ZPAUTO variable, unknown until allocation
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


// An evaluation error riding in a value: the code, plus an optional detail string the diagnostic
// machinery can surface (today: the symbol NAME behind an unknown_symbol, so "Undefined symbol: 'x'"
// can say which - the evaluator is the only place that still knows it). {0} detail when there is none.
typedef struct value_error {
    error_type code;
    rc_str     detail;
} value_error;


// The address of a ZPAUTO variable before allocation has chosen it. Real addresses exist only after
// the whole program has converged, so during assembly a variable's symbol carries THIS instead of a
// number: the variable's identity - the (declaring scope, ZPAUTO def cursor) pair the zero-page IR
// keys on - plus a byte offset within it (`ptr+1` is the same identity at offset 1). Contexts that
// genuinely need a number (a count, a condition, a layout address) refuse it with a clear error;
// instruction operands and data emissions accept it and receive the real address on the output pass.
typedef struct value_zpauto {
    uint32_t scope;     // the variable's declaring scope index...
    cursor   def;       // ...and its ZPAUTO statement's def cursor: together, its identity
    int32_t  offset;    // byte offset within the variable
    rc_str   name;      // the variable's name, for diagnostics (view into permanent backing)
} value_zpauto;

struct value {
    value_type type;
    union {
        double       numeric;           // also the boolean payload, canonically 1.0 / 0.0
        rc_str       string;            // view into source or arena; concat allocates
        value_list   list;              // rc_view_value: const value *, num
        value_range  range;
        value_error  error;
        value_zpauto zpauto;
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
value value_make_bool(bool b);
value value_make_string(rc_str s);
value value_make_error(error_type e);
value value_make_error_detail(error_type e, rc_str detail);   // + a payload string (e.g. the symbol name)
value value_make_range(value_range r);
value value_make_list(rc_view_value items);    // wraps the view; does not copy
value value_make_zpauto(uint32_t scope, cursor def, int32_t offset, rc_str name);

// Deep-copy v into arena so it can outlive the (scratch) arena it was built in. A value
// is a non-owning handle, so a plain copy still points at the old backing; this is the
// deliberate promotion across the scratch-to-permanent boundary. Scalars (none, numeric,
// range, error) have no backing and copy as-is; a string copies its bytes; a list copies
// its element storage and recurses into each element.
value value_make_copy(value v, rc_arena *arena);

// Range builders. They take the (already evaluated) endpoint values and own all the
// range semantics: step inference, the second-element/stepped form (rhs is a range),
// exclusivity, and validation. Each returns a range value, or a propagating error
// value (a bad endpoint, a non-integer, an empty or inconsistent range).
value value_make_range_pair(value lhs, value rhs, bool exclusive);   // lhs..rhs / lhs..<rhs
value value_make_range_open_end(value lhs, bool exclusive);          // lhs..
value value_make_range_open_start(value rhs, bool exclusive);        // ..rhs / ..<rhs
value value_make_range_open(bool exclusive);                         // ..

// The effective step of a range: the stored .step, or the inferred +-1 direction when
// .step is 0 (the "infer the direction" convention). An open end infers ascending.
int64_t value_range_step(value_range r);

// Type queries.
value_type value_type_of(value v);
bool value_is_none(value v);
bool value_is_numeric(value v);
bool value_is_boolean(value v);
bool value_is_number(value v);     // numeric or boolean: anything that coerces to a number
bool value_is_string(value v);
bool value_is_list(value v);
bool value_is_range(value v);
bool value_is_error(value v);
bool value_is_zpauto(value v);
bool value_is_simple(value v);     // numeric, boolean, string, error, or zpauto: stands alone
bool value_is_compound(value v);   // list or range: gathers other values

// Structural equality: types must match, then compared field-wise (lists
// element-wise, recursively).
bool value_is_equal(value a, value b);

// Diagnostics.
void value_format(rc_mstr *out, value v, rc_arena *arena);


#endif // ifndef BARON_VALUE_H_
