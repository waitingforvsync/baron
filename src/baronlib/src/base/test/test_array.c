#include "base/test.h"
#include "base/allocator.h"
#include "base/array.h"

DEF_TEST(array, common_ops)
{
    array_int32_t arr = make_array(int32_t, allocator_default(), 4);
    REQUIRE_TRUE(array_is_valid(&arr));
    REQUIRE(array_capacity(&arr),==,4);
    REQUIRE_TRUE(array_resize(&arr, 2));
    REQUIRE(arr.size,==,2);
    REQUIRE_TRUE(arr.data != 0);

    REQUIRE_TRUE(array_add(&arr, 25));
    REQUIRE(arr.size,==,3);
    REQUIRE(arr.data[2],==,25);

    REQUIRE_TRUE(array_add(&arr, 42));
    REQUIRE(arr.size,==,4);
    REQUIRE(array_capacity(&arr),==,arr.size);

    int32_t *oldptr = arr.data;
    REQUIRE_TRUE(array_add(&arr, 69));
    REQUIRE_TRUE(arr.data != oldptr);
    REQUIRE(arr.size,==,5);
    REQUIRE(array_capacity(&arr),>,arr.size);

    array_deinit(&arr);
    REQUIRE(arr.size,==,0);
    REQUIRE_TRUE(arr.data == 0);
}

DEF_TEST(array, append)
{
    array_int32_t arr = make_array(int32_t, allocator_default(), 4);
    array_add(&arr, 1);
    array_add(&arr, 2);
    array_add(&arr, 3);
    array_add(&arr, 4);

    array_int32_t copy = make_array(int32_t, allocator_default(), 2);
    REQUIRE_FALSE(array_contains_slice(&copy, arr.const_slice));
    REQUIRE_TRUE(array_append(&copy, arr.const_slice));
    REQUIRE(copy.size,==,4);
    REQUIRE(array_capacity(&copy),==,4);
    REQUIRE(copy.data[0],==,1);
    REQUIRE(copy.data[1],==,2);
    REQUIRE(copy.data[2],==,3);
    REQUIRE(copy.data[3],==,4);

    array_deinit(&copy);
    array_deinit(&arr);
}
