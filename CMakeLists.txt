cmake_minimum_required(VERSION 3.14)
project(kinarm_binary_converter CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(kinarm-binary-converter
    src/kinarm_converter.cpp
    src/miniz.c
)
target_include_directories(kinarm-binary-converter PRIVATE src/)

if(MSVC)
  target_compile_options(kinarm-binary-converter PRIVATE /W3 /O2)
  set_source_files_properties(src/miniz.c PROPERTIES COMPILE_FLAGS "/TC")
else()
  target_compile_options(kinarm-binary-converter PRIVATE -Wall -O2)
  set_source_files_properties(src/miniz.c PROPERTIES COMPILE_FLAGS "-std=c99")
endif()
