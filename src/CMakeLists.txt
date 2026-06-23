add_executable(baron
    main.c
    value.c
    value.h
    lexeme.h
    token.c
    token.h
    lexer.c
    lexer.h
    scopes.c
    scopes.h
    expression.c
    expression.h
    baron.c
    baron.h
)

target_link_libraries(baron PRIVATE richc)
target_include_directories(baron PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Tests are compiled into the binary (so they can reach internal/static
# functions) and run after every build; a failing RC_CHECK fails the build.
target_compile_definitions(baron PRIVATE BARON_TESTS)

if(MSVC)
    target_compile_options(baron PRIVATE /W3)
else()
    target_compile_options(baron PRIVATE -Wall -Wextra)
endif()

add_custom_command(
    TARGET baron
    POST_BUILD
    COMMAND "$<TARGET_FILE:baron>" --test
    COMMENT "Running baron unit tests"
)
