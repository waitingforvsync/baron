#include "base/test.h"
#include "base/allocator.h"
#include "base/array.h"

DEF_TEST(array, init)
{
    array_int32_t arr = make_array(int32_t, allocator_default(), 4);
    REQUIRE(array_capacity(&arr),==,4);
    array_resize(&arr, 2);
    REQUIRE(arr.size,==,2);
    REQUIRE_TRUE(arr.data != 0);

    int error = array_add(&arr, 25);
    REQUIRE(error,==,0);
    REQUIRE(arr.size,==,3);
    REQUIRE(arr.data[2],==,25);

    error = array_add(&arr, 42);
    REQUIRE(error,==,0);
    REQUIRE(arr.size,==,4);
    REQUIRE(array_capacity(&arr),==,arr.size);

    int32_t *oldptr = arr.data;
    error = array_add(&arr, 69);
    REQUIRE(error,==,0);
    REQUIRE_TRUE(arr.data != oldptr);
    REQUIRE(arr.size,==,5);
    REQUIRE(array_capacity(&arr),>,arr.size);
}
