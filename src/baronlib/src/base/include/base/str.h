/**
 *  @file   str.h
 * 
 *  String handling support
 */

#ifndef STR_H_
#define STR_H_

#include <stdbool.h>
#include <stdint.h>
#include "array.h"

typedef array_uint8_t strbuf_t;
typedef slice_const_uint8_t str_t;
typedef struct str_pair_t str_pair_t;

struct str_pair_t {
    str_t first;
    str_t second;
};


/**
 *	Make a str_t from a C string literal
 */
#define STR(s) \
	_Generic((&(s)), \
		char(*)[sizeof s]: ((str_t){(const uint8_t *)(s), (uint32_t)(sizeof s) - 1}), \
		const char(*)[sizeof s]: ((str_t){(const uint8_t *)(s), (uint32_t)(sizeof s) - 1}) \
	)


/**
 *	Make a str_t from an arbitrary char *
 */
str_t str_from_chars(const char *s);


/**
 *	Is a str_t valid?
 */
static inline bool str_is_valid(str_t str) {
	return str.data;
}

/**
 * Get the length of the str_t
 */
static inline uint32_t str_length(str_t str) {
	return str.size;
}


/**
 *	Is str_t a equal to str_t b?
 */
bool str_equal(str_t a, str_t b);


/**
 *	Compare str_t a with str_t b
 */
int str_compare(str_t a, str_t b);


/**
 *	Does str_t a contain str_t b?
 */
bool str_contains(str_t a, str_t b);


/**
 *	Get the leftmost chars of a str_t
 */
str_t str_left(str_t s, uint32_t count);


/**
 *	Get the rightmost chars of a str_t
 */
str_t str_right(str_t s, uint32_t count);


/**
 *	Get a substring of a str_t
 */
str_t str_substr(str_t s, uint32_t start, uint32_t count);


/**
 *	Get a str_t from the middle to the end
 */
str_t str_mid(str_t s, uint32_t start);


/**
 *	Does str_t a start with str_t b?
 */
bool str_startswith(str_t a, str_t b);


/**
 *	Does str_t a end with str_t b?
 */
bool str_endswith(str_t a, str_t b);


/**
 *	Find the first instance of str_t b in str_t a and return its index
 */
uint32_t str_find_first(str_t a, str_t b);


/**
 *	Find the last instance of str_t b in str_t a and return its index
 */
uint32_t str_find_last(str_t a, str_t b);


/**
 *	Return the str_t with the given prefix removed if possible
 */
str_t str_remove_prefix(str_t src, str_t prefix);


/**
 *	Return the str_t with the given suffix removed if possible
 */
str_t str_remove_suffix(str_t src, str_t suffix);


/**
 *	Splits the given str_t by a given substring from the front, and returns the head/tail pair
 */
str_pair_t str_first_split(str_t src, str_t split_by);


/**
 *	Splits the given str_t by a given substring from the back, and returns the head/tail pair
 */
str_pair_t str_last_split(str_t src, str_t split_by);


/**
 *	Macro to output {length, data} parameters to a %.*s printf formatter, to print a cstr
 */
#define STR_PRINT(s) (s).size, (s).data
#define STR_FORMAT "%.*s"


#endif // STR_H_
