cmake_minimum_required(VERSION 3.1)
project(atlasc)

option(STATIC_LIB "Build atlasc as static-library instead of command-line tool" OFF)

add_definitions(-D__STDC_LIMIT_MACROS)
add_definitions(-D__STDC_FORMAT_MACROS)
add_definitions(-D__STDC_CONSTANT_MACROS)
if(MSVC)
    add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
    add_definitions(-D_HAS_EXCEPTIONS=0)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS=0)
endif()
if (RPI)
    add_definitions(-D__RPI__)
endif()

add_subdirectory(3rdparty/sx)
add_subdirectory(3rdparty/delaunay)
add_executable(atlasc src/atlasc.c)
target_link_libraries(atlasc PRIVATE sx delaunay)
target_include_directories(atlasc PRIVATE 3rdparty)

