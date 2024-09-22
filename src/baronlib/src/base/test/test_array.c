#include "base/test.h"
#include "base/allocator.h"
#include "base/array.h"

DEF_TEST(array, init)
{
    array_int32_t arr = make_array(int32_t, allocator_default(), 4);
    REQUIRE(array_capacity(&arr),==,4);
    REQUIRE(arr.size,==,0);
    REQUIRE(arr.data != 0,==,true);
}
