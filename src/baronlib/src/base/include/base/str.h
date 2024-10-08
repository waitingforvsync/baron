/**
 *  @file   str.h
 * 
 *  String handling support
 */

#ifndef STR_H_
#define STR_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct str_t str_t;
typedef struct strview_t strview_t;

struct strview_t {
	const uint8_t *data;
	uint32_t length;
};

struct str_t {
	union {
		struct {
			uint8_t *data;
			uint32_t length;
		};
		strview_t view;
	};
};



/**
 *	Make a strview_t from a C string literal
 */
#define STRVIEW(s) \
	_Generic((&(s)), \
		char(*)[sizeof s]: ((strview_t){(const uint8_t *)(s), (uint32_t)(sizeof s) - 1}), \
		const char(*)[sizeof s]: ((strview_t){(const uint8_t *)(s), (uint32_t)(sizeof s) - 1}) \
	)


/**
 *  Make an empty str with the given capacity
 * 
 *  @param	capacity		The initial capacity that will be reserved for the string
 * 
 *  @return A new str_t instance.
 *          If allocation failed, the str is invalid.
 */
str_t make_str(uint32_t capacity);


/**
 *  Check validity of a str
 * 
 *  @param	str				Pointer to the str to check validity
 * 
 *  @return Whether valid, true/false
 */
static inline bool str_is_valid(const str_t *str) {
	return str && str->data;
}


/**
 *  Reset a str to empty, without changing any existing allocations.
 * 
 *  @param	str				Pointer to the str to reset
 */
void str_reset(str_t *str);


/**
 *  Determine whether a str is empty or not.
 * 
 *  @param	str				Pointer to the str to query
 * 
 *  @return	Whether empty, true/false
 */
bool str_is_empty(const str_t *str);


/**
 *  Deinitialize a str, freeing all allocations
 * 
 *  @param	str				Pointer to the str to deinitialize
 */
void str_deinit(str_t *str);


/**
 *  Append a strview to the given str
 * 
 *  @param	str				Pointer to the str to be appended to
 *  @param	append_str		strview to be appended
 * 
 *  @return	Whether the append was successful, true/false
 */
bool str_append(str_t *str, strview_t append_str);


/**
 *	Make a strview_t from an arbitrary char *
 *
 *  @param	s			A C-string pointer
 * 
 *  @return a strview_t referencing the C-string (minus the zero terminator)
 */
strview_t make_strview(const char *s);


/**
 *	Is a str_t valid?
 *
 *  @param	str			The strview to check for validity
 * 
 *  @return	valid true/false
 */
static inline bool strview_is_valid(strview_t str) {
	return str.data != 0;
}


/**
 *	Return whether two strviews are equal
 * 
 *  @param	a			First strview to compare
 *  @param	b			Second strview to compare
 *
 *  @return strviews are equal, true/false
 */
bool strview_equal(strview_t a, strview_t b);


/**
 *	Compare strview a with strview b
 * 
 *  @param	a			First strview to compare
 *  @param	b			Second strview to compare
 *
 *  @return 0 if the strings are lexographically equal
 *          <0 if string a is lexographically before string b
 *          >0 if string a is lexographically after string b
 */
int strview_compare(strview_t a, strview_t b);


/**
 *	Get the leftmost chars of a strview
 */
strview_t strview_left(strview_t s, uint32_t count);


/**
 *	Get the rightmost chars of a strview
 */
strview_t strview_right(strview_t s, uint32_t count);


/**
 *	Get a substring of a strview
 */
strview_t strview_substr(strview_t s, uint32_t start, uint32_t count);


/**
 *	Get a strview from the middle to the end
 */
strview_t strview_mid(strview_t s, uint32_t start);


/**
 *	Does strview a start with strview b?
 *
 *  @param	a			strview to test
 *  @param	b			prefix to test for
 *
 *  @return true/false
 */
bool strview_startswith(strview_t a, strview_t b);


/**
 *	Does strview a end with strview b?
 *
 *  @param	a			strview to test
 *  @param	b			suffix to test for
 *
 *  @return true/false
 */
bool strview_endswith(strview_t a, strview_t b);


/**
 *	Find the first instance of strview b in strview a and return its index
 */
uint32_t strview_find_first(strview_t a, strview_t b);


/**
 *	Find the last instance of strview b in strview a and return its index
 */
uint32_t strview_find_last(strview_t a, strview_t b);


/**
 *	Does strview a contain strview b?
 * 
 *  @param	a			strview to query
 *  @param	b			Substring to check
 *
 *  @return true/false
 */
bool strview_contains(strview_t a, strview_t b);


/**
 *	Return the strview with the given prefix removed if possible
 */
strview_t strview_remove_prefix(strview_t src, strview_t prefix);


/**
 *	Return the strview with the given suffix removed if possible
 */
strview_t strview_remove_suffix(strview_t src, strview_t suffix);


typedef struct strview_pair_t strview_pair_t;
struct strview_pair_t {
    strview_t first;
    strview_t second;
};

/**
 *	Splits the given strview by a given substring from the front, and returns the head/tail pair
 */
strview_pair_t strview_first_split(strview_t src, strview_t split_by);


/**
 *	Splits the given strview by a given substring from the back, and returns the head/tail pair
 */
strview_pair_t strview_last_split(strview_t src, strview_t split_by);


typedef struct strview_parse_int_result_t strview_parse_int_result_t;
struct strview_parse_int_result_t {
	int64_t value;
	uint32_t length_parsed;
};

/**
 *  Parses the given strview as a decimal numeric value
 */
strview_parse_int_result_t strview_parse_int(strview_t src);


typedef struct strview_parse_hex_result_t strview_parse_hex_result_t;
struct strview_parse_hex_result_t {
	uint64_t value;
	uint32_t length_parsed;
};

/**
 *  Parses the given strview as a hex numeric value
 */
strview_parse_hex_result_t strview_parse_hex(strview_t src);


/**
 *	Macro to output {length, data} parameters to a %.*s printf formatter, to print a cstr
 */
#define STR_PRINT(s) (s).length, (s).data
#define STR_FORMAT "%.*s"


#endif // STR_H_
