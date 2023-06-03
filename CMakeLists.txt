cmake_minimum_required(VERSION 3.4.1)

project(StackAllocator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT COMMAND testBuilder_build)
    add_subdirectory(testBuilder)
    testBuilder_set_current_working_directory_to_default_binary_directory()
endif()

testBuilder_add_include(StackAllocator include)
testBuilder_add_source(StackAllocator src/log.cpp)
testBuilder_build_shared_library(StackAllocator)

testBuilder_add_source(stack_exe src/executable.cpp)
testBuilder_add_library(stack_exe StackAllocator)
testBuilder_build(stack_exe EXECUTABLES)
