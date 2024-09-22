cmake_minimum_required(VERSION 3.24)
project("Baron")
set(CMAKE_C_STANDARD 11)

if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-unused-but-set-variable)
endif()

add_subdirectory("baron")
add_subdirectory("baronlib")
