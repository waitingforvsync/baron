target_sources("base"
    PUBLIC
    "allocator.h"
    "array.h"
    "defines.h"
    "str.h"
    "test.h"
)

target_include_directories("base" PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
