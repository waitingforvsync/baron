add_executable("baron")
target_link_libraries("baron" PRIVATE "baronlib")

target_sources("baron"
    PRIVATE
    "main.c"
)
