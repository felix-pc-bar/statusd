cmake_minimum_required(VERSION 3.22)
project(statusd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Release by default; override with -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(statusd
    src/main.cpp
)

target_include_directories(statusd PRIVATE include)

target_compile_options(statusd PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g3 -fsanitize=address,undefined>
)

target_link_options(statusd PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

install(TARGETS statusd DESTINATION bin)
install(FILES   statusd.service
        DESTINATION lib/systemd/system)
