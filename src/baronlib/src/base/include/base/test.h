#ifndef TEST_H_
#define TEST_H_

#include "base/defines.h"
#include "base/str.h"


typedef struct test_item test_item;

struct test_item {
    str_t group_name;
	str_t test_name;
	void (*test_fn)(void);
	void (*test_group_fn)(void *);
	void (**init_fn)(void *);
	void (**deinit_fn)(void *);
	void *context;
};


#define DEF_TEST(group, test) \
	static void test_##group##_##test(void); \
	static const test_item test_item_##group##_##test = { \
		.group_name = {(const uint8_t *)#group, (uint32_t)(sizeof #group) - 1}, \
		.test_name = {(const uint8_t *)#test, (uint32_t)(sizeof #test) - 1}, \
		.test_fn = test_##group##_##test \
	}; \
    SECTION("test$b") const test_item *test_item_ptr_##group##_##test = &test_item_##group##_##test; \
	static void test_##group##_##test(void)

#define DEF_TEST_SKIP(group, test) \
	UNUSED_FN static void test_##group##_##test(void); \
	static const eq_test_item test_item_##group##_##test = { \
		.group_name = {(const uint8_t *)#group, (uint32_t)(sizeof #group) - 1}, \
		.test_name = {(const uint8_t *)#test, (uint32_t)(sizeof #test) - 1}, \
	}; \
    SECTION("test$b") const test_item *test_item_ptr_##group##_##test = &test_item_##group##_##test; \
	static void test_##group##_##test(void)

#define EQ_TEST_DEF_GROUP_DATA(group) \
	struct test_group_data_##group; \
	static void (*test_group_initfn_##group)(struct test_group_data_##group *); \
	static void (*test_group_deinitfn_##group)(struct test_group_data_##group *); \
	struct test_group_data_##group 

#define EQ_TEST_GROUP_INIT(group) \
	static void test_group_init_##group(struct test_group_data_##group *data); \
	static void (*test_group_initfn_##group)(struct test_group_data_##group *) = &test_group_init_##group; \
	static void test_group_init_##group(struct test_group_data_##group *data)

#define EQ_TEST_GROUP_DEINIT(group) \
	static void test_group_deinit_##group(struct test_group_data_##group *data); \
	static void (*test_group_deinitfn_##group)(struct test_group_data_##group *) = &test_group_deinit_##group; \
	static void test_group_deinit_##group(struct test_group_data_##group *data)

#define EQ_TEST_WITH_FIXTURE(group, test) \
	static void test_##group##_##test(struct test_group_data_##group *); \
	static struct test_group_data_##group test_group_data_##group##_##test; \
	static const test_item test_item_##group##_##test = { \
		.group_name = {(const uint8_t *)#group, (uint32_t)(sizeof #group) - 1}, \
		.test_name = {(const uint8_t *)#test, (uint32_t)(sizeof #test) - 1}, \
		.test_group_fn = (void (*)(void *))test_##group##_##test, \
		.init_fn = (void(**)(void *))&test_group_initfn_##group, \
		.deinit_fn = (void(**)(void *))&test_group_deinitfn_##group, \
		.context = &test_group_data_##group##_##test \
	}; \
    SECTION("test$b") const test_item *test_item_ptr_##group##_##test = &test_item_##group##_##test; \
	static void test_##group##_##test(struct test_group_data_##group *data)

#define EQ_TEST_WITH_FIXTURE_SKIP(group, test) \
	UNUSED_FN static void test_##group##_##test(struct test_group_data_##group *); \
	static struct test_group_data_##group test_group_data_##group##_##test; \
	static const test_item test_item_##group##_##test = { \
		.group_name = {(const uint8_t *)#group, (uint32_t)(sizeof #group) - 1}, \
		.test_name = {(const uint8_t *)#test, (uint32_t)(sizeof #test) - 1}, \
		.context = &test_group_data_##group##_##test \
	}; \
    SECTION("test$b") const test_item *test_item_ptr_##group##_##test = &test_item_##group##_##test; \
	static void test_##group##_##test(struct test_group_data_##group *data)


#define REQUIRE(a, op, b) REQUIRE_IMPL(a, op, b, __FILE__, __LINE__)

#define REQUIRE_IMPL(a, op, b, file, line) \
	_Generic((a), \
		bool: test_require_bool, \
		int8_t: test_require_int, \
		uint8_t: test_require_int, \
		int16_t: test_require_int, \
		uint16_t: test_require_int, \
		int32_t: test_require_int, \
		uint32_t: test_require_int, \
		int64_t: test_require_int, \
		uint64_t: test_require_int, \
		float: test_require_float, \
		double: test_require_float, \
		str_t: test_require_str \
	)(a, #op, b, #a " " #op " " #b, file, line)

void test_require_bool(bool actual, const char *op, bool expected, const char *expr, const char *file, int line);
void test_require_int(int64_t actual, const char *op, int64_t expected, const char *expr, const char *file, int line);
void test_require_float(double actual, const char *op, double expected, const char *expr, const char *file, int line);
void test_require_str(str_t actual, const char *op, str_t expected, const char *expr, const char *file, int line);

int test_run(const char *filter);


#endif // ifndef TEST_H_
