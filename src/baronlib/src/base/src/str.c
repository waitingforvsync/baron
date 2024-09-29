#include <string.h>
#include "base/defines.h"
#include "base/str.h"



strview_t make_strview(const char *s) {
	return (strview_t){(const uint8_t *)s, (uint32_t)strlen(s)};
}


bool strview_equal(strview_t a, strview_t b) {
	ASSERT(strview_is_valid(a));
	ASSERT(strview_is_valid(b));
	return a.length == b.length && memcmp(a.data, b.data, a.length) == 0;
}


int strview_compare(strview_t a, strview_t b) {
	ASSERT(strview_is_valid(a));
	ASSERT(strview_is_valid(b));
	int result = memcmp(a.data, b.data, math_min_uint32(a.length, b.length));
	if (result == 0) {
		result += (a.length > b.length) - (b.length > a.length);
	}
	return result;
}


strview_t strview_left(strview_t s, uint32_t count) {
	ASSERT(strview_is_valid(s));
	return (strview_t){s.data, math_min_uint32(count, s.length)};
}


strview_t strview_right(strview_t s, uint32_t count) {
	ASSERT(strview_is_valid(s));
	count = math_min_uint32(count, s.length);
	return (strview_t){s.data + s.length - count, count};
}


strview_t strview_substr(strview_t s, uint32_t start, uint32_t count) {
	ASSERT(strview_is_valid(s));
	start = math_min_uint32(start, s.length);
	count = math_min_uint32(count, s.length - start);
	return (strview_t){s.data + start, count};
}


strview_t strview_mid(strview_t s, uint32_t start) {
	ASSERT(strview_is_valid(s));
	start = math_min_uint32(start, s.length);
	return (strview_t){s.data + start, s.length - start};
}


bool strview_startswith(strview_t a, strview_t b) {
	ASSERT(strview_is_valid(a));
	ASSERT(strview_is_valid(b));
	if (a.length < b.length) {
		return false;
	}
	return (memcmp(a.data, b.data, b.length) == 0);
}


bool strview_endswith(strview_t a, strview_t b) {
	ASSERT(strview_is_valid(a));
	if (a.length < b.length) {
		return false;
	}
	return (memcmp(a.data + a.length - b.length, b.data, b.length) == 0);
}


uint32_t strview_find_first(strview_t a, strview_t b) {
	uint32_t count = (a.length >= b.length) ? a.length - b.length + 1 : 0;
	for (uint32_t i = 0; i < count; i++) {
		if (memcmp(a.data + i, b.data, b.length) == 0) {
			return i;
		}
	}
	return invalid_index;
}


uint32_t strview_find_last(strview_t a, strview_t b) {
	uint32_t start = (a.length >= b.length) ? a.length - b.length + 1 : 0;
	for (uint32_t i = start; i-- > 0; ) {
		if (memcmp(a.data + i, b.data, b.length) == 0) {
			return i;
		}
	}
	return invalid_index;
}


bool strview_contains(strview_t a, strview_t b) {
	return strview_find_first(a, b) != invalid_index;
}


strview_t strview_remove_prefix(strview_t src, strview_t prefix) {
	if (strview_startswith(src, prefix)) {
		return strview_mid(src, prefix.length);
	}
	return src;
}


strview_t strview_remove_suffix(strview_t src, strview_t suffix) {
	if (strview_endswith(src, suffix)) {
		return strview_left(src, src.length - suffix.length);
	}
	return src;
}


strview_pair_t strview_first_split(strview_t src, strview_t split_by) {
	uint32_t index = strview_find_first(src, split_by);
	if (index == invalid_index) {
		return (strview_pair_t){src, (strview_t){0}};
	}
	return (strview_pair_t){
		strview_left(src, index),
		strview_mid(src, index + split_by.length)
	};
}


strview_pair_t strview_last_split(strview_t src, strview_t split_by) {
	uint32_t index = strview_find_last(src, split_by);
	if (index == invalid_index) {
		return (strview_pair_t){(strview_t){0}, src};
	}
	return (strview_pair_t){
		strview_left(src, index),
		strview_mid(src, index + split_by.length)
	};
}
