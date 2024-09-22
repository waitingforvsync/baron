add_library("base" STATIC)
target_compile_definitions("base" PUBLIC WITH_TESTS=1)

add_subdirectory("include")
add_subdirectory("src")
add_subdirectory("test")
