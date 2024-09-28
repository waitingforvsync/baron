/**
 *  @file   test.c
 * 
 *  Basic unit testing implementation
 */

#include "base/test.h"
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


#if COMPILER_CLANG || COMPILER_MSVC
SECTION("test$a") static const test_item *__start_test;
SECTION("test$c") static const test_item *__stop_test;
#endif

#if COMPILER_GCC
extern const test_item *__start_test[];
extern const test_item *__stop_test[];
#endif


static jmp_buf env;
static double epsilon = 0.0001;

static bool is_op1(const char *a, const char *b) {
	return a[0] == b[0];
}

static bool is_op2(const char *a, const char *b) {
	return a[0] == b[0] && a[1] == b[1];
}

void test_require_bool(bool actual, const char *op, bool expected, const char *expr, const char *file, int line) {
	if (is_op2(op, "==")) { if (actual == expected) return; } else
	if (is_op2(op, "!=")) { if (actual != expected) return; }
	else {
		printf("[FAIL]\n%s:%d: Unsupported operation: %s\n", file, line, op);
	}
	printf("[FAIL]\n%s:%d: %s: actual %s\n", file, line, expr, actual ? "true" : "false");
	longjmp(env, 1);
}

void test_require_int(int64_t actual, const char *op, int64_t expected, const char *expr, const char *file, int line) {
	if (is_op2(op, "==")) { if (actual == expected) return; } else
	if (is_op2(op, "!=")) { if (actual != expected) return; } else
	if (is_op1(op, "<" )) { if (actual <  expected) return; } else
	if (is_op1(op, ">" )) { if (actual >  expected) return; } else
	if (is_op2(op, "<=")) { if (actual <= expected) return; } else
	if (is_op2(op, ">=")) { if (actual >= expected) return; }
	else {
		printf("[FAIL]\n%s:%d: Unsupported operation: %s\n", file, line, op);
	}
	printf("[FAIL]\n%s:%d: %s: actual %lld\n", file, line, expr, actual);
	longjmp(env, 1);
}

void test_require_float(double actual, const char *op, double expected, const char *expr, const char *file, int line) {
	if (is_op2(op, "==")) { if (actual == expected) return; } else
    if (is_op2(op, "~=")) { if (fabs(actual - expected) < epsilon) return; } else
	if (is_op2(op, "!=")) { if (actual != expected) return; } else
	if (is_op1(op, "<" )) { if (actual <  expected) return; } else
	if (is_op1(op, ">" )) { if (actual >  expected) return; } else
	if (is_op2(op, "<=")) { if (actual <= expected) return; } else
	if (is_op2(op, ">=")) { if (actual >= expected) return; }
	else {
		printf("[FAIL]\n%s:%d: Unsupported operation: %s\n", file, line, op);
	}
	printf("[FAIL]\n%s:%d: %s: actual %f\n", file, line, expr, actual);
	longjmp(env, 1);
}

void test_require_str(str_t actual, const char *op, str_t expected, const char *expr, const char *file, int line) {
	if (is_op2(op, "==")) { if (str_equal(actual, expected)) return; } else
	if (is_op2(op, "!=")) { if (!str_equal(actual, expected)) return; }
	else {
		printf("[FAIL]\n%s:%d: Unsupported operation: %s\n", file, line, op);
	}
	printf("[FAIL]\n%s:%d: %s: actual \"" STR_FORMAT "\"\n", file, line, expr, STR_PRINT(actual));
	longjmp(env, 1);
}


int test_run(const char *filter_string) {
    str_t filter = str_from_chars(filter_string);
	int num_fail = 0;
	int num_pass = 0;
	int num_skip = 0;

	// Count number of tests we're going to run
	int total = 0;
	int max_width = 0;
	for (const test_item **start = &__start_test; start != &__stop_test; start++) {
		const test_item *test_item = *start;
		if (test_item && str_startswith(test_item->group_name, filter)) {
            int width = test_item->group_name.size + test_item->test_name.size;
            if (width > max_width) {
                max_width = width;
            }
			total++;
		}
	}
	
	// Iterate through tests
	int count = 1;
	for (const test_item **start = &__start_test; start != &__stop_test; start++) {
		const test_item *test_item = *start;
		
		if (test_item && str_startswith(test_item->group_name, filter)) {
			int32_t padding = max_width + 3 - test_item->group_name.size - test_item->test_name.size;
            if (padding < 0) {
                padding = 0;
            }
			
			printf("%4d/%d:  " STR_FORMAT "." STR_FORMAT "%-*s", 
				count, total, STR_PRINT(test_item->group_name), STR_PRINT(test_item->test_name), padding, ":");
			fflush(stdout);
			
			if (test_item->test_fn || test_item->test_group_fn) {
			
				if (!setjmp(env)) {
					// Call the group init function if one defined
					if (test_item->init_fn && *test_item->init_fn) {
						(*test_item->init_fn)(test_item->context);
					}

					// Call the test, with or without fixture context as appropriate
					if (test_item->context) {
						test_item->test_group_fn(test_item->context);
					}
					else {
						test_item->test_fn();
					}
					
					// Call the group deinit function if one defined
					if (test_item->deinit_fn && *test_item->deinit_fn) {
						(*test_item->deinit_fn)(test_item->context);
					}
					
					// If we got here, it's a pass
					printf("ok\n");
					num_pass++;
				}
				else {
					// If we got here it's a fail
					num_fail++;
				}
			}
			else {
				// If we got here, we skipped
				printf("[SKIP]\n");
				num_skip++;
			}
			
			count++;
		}
	}
	
	printf("\nRESULTS: %d tests (%d ok, %d failed, %d skipped)\n", total, num_pass, num_fail, num_skip);
	fflush(stdout);
	return num_fail;
}

