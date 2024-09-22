add_library("baronlib" STATIC)
target_compile_definitions("baronlib" PUBLIC WITH_TESTS=1)

add_subdirectory("include")
add_subdirectory("src")
