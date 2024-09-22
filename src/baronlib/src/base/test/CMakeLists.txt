add_executable("base_tests")
target_link_libraries("base_tests" PRIVATE "base")

target_sources("base_tests"
    PRIVATE
    "main.c"
    "test_array.c"
    "test_str.c"
)

add_custom_command(
    TARGET "base_tests"
    COMMENT "Run tests"
    POST_BUILD
    COMMAND "base_tests"
)
