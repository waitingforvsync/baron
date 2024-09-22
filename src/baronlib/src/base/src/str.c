#include <string.h>
#include "base/defines.h"
#include "base/str.h"



str_t str_from_chars(const char *s) {
	return (str_t){(const uint8_t *)s, (uint32_t)strlen(s)};
}


bool str_equal(str_t a, str_t b) {
	ASSERT(str_is_valid(a));
	ASSERT(str_is_valid(b));
	return a.size == b.size && memcmp(a.data, b.data, a.size) == 0;
}


bool str_startswith(str_t a, str_t b) {
	ASSERT(str_is_valid(a));
	if (a.size < b.size) {
		return false;
	}
	
	if (b.size == 0) {
		return true;
	}
	
	return (memcmp(a.data, b.data, b.size) == 0);
}
