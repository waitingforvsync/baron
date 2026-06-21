cmake_minimum_required(VERSION 3.21)
project(baron C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# richc dependency (git submodule at extern/richc). Core library only: the app
# layer needs GLFW/glad and is not used by baron. RICHC_CORE_ONLY is set as a
# normal variable, honoured over richc's option() default via CMP0077.
set(RICHC_CORE_ONLY ON)
add_subdirectory(extern/richc)

add_subdirectory(src)
