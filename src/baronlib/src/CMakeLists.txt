target_sources("baronlib"
    PRIVATE
    "baron.c"
)

add_subdirectory("base")
target_link_libraries("baronlib" PRIVATE "base")
target_include_directories("baronlib" PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
