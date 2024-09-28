target_sources("base"
    PUBLIC
    "allocator.h"
    "arena.h"
    "array.h"
    "bitarray.h"
    "defines.h"
    "file.h"
    "str.h"
    "test.h"
)

target_include_directories("base" PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
