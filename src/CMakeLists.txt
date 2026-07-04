add_executable(baron
    main.c
    value.c
    value.h
    error.c
    error.h
    lexeme.h
    token.c
    token.h
    lexer.c
    lexer.h
    cursor.h
    scopes.c
    scopes.h
    expression.c
    expression.h
    opcodes.c
    opcodes.h
    overlays.c
    overlays.h
    source_files.c
    source_files.h
    file_utils.c
    file_utils.h
    assemble.c
    assemble.h
    assemble_internal.h
    baron.c
    baron.h
)

target_link_libraries(baron PRIVATE richc)
target_include_directories(baron PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(baron PRIVATE /W3)
else()
    target_compile_options(baron PRIVATE -Wall -Wextra)
endif()

# Gated by the BARON_TESTS option (default ON). Tests are compiled into the binary
# (so they can reach internal/static functions) and run after every build; a failing
# RC_CHECK fails the build. They run from a dedicated working directory, with the test
# data files (src/test) copied in first, so file I/O tests use relative names without
# touching platform-specific locations.
if(BARON_TESTS)
    target_compile_definitions(baron PRIVATE BARON_TESTS)
    set(BARON_TEST_WORKDIR "${CMAKE_CURRENT_BINARY_DIR}/test-workdir")
    file(MAKE_DIRECTORY "${BARON_TEST_WORKDIR}")
    add_custom_command(
        TARGET baron
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/test" "${BARON_TEST_WORKDIR}"
        COMMAND "$<TARGET_FILE:baron>" --test
        WORKING_DIRECTORY "${BARON_TEST_WORKDIR}"
        COMMENT "Running baron unit tests"
    )
endif()
