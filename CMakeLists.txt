cmake_minimum_required(VERSION 3.4.1)

project(StackAllocator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT COMMAND testBuilder_build)
    add_subdirectory(testBuilder)
    testBuilder_set_current_working_directory_to_default_binary_directory()
endif()

add_subdirectory(alloc_hook)

if (true)
    testBuilder_add_include(StackAllocator include)
    testBuilder_add_source(StackAllocator src/empty.cpp)
    testBuilder_add_source(StackAllocator src/log.cpp)
    testBuilder_build_shared_library(StackAllocator)

    testBuilder_add_include(StackAllocatorL include)
    testBuilder_add_source(StackAllocatorL src/empty.cpp)
    testBuilder_add_source(StackAllocatorL src/log.cpp)
    testBuilder_add_compile_option(StackAllocatorL "SHELL:-D SA_STACK_ALLOCATOR__LOGGING=1")
    testBuilder_build_shared_library(StackAllocatorL)

    testBuilder_add_include(StackAllocatorOverride include)
    testBuilder_add_source(StackAllocatorOverride src/empty.cpp)
    testBuilder_add_library(StackAllocatorOverride StackAllocator)
    testBuilder_add_compile_option(StackAllocatorOverride "SHELL:-D SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW=1")
    testBuilder_build_shared_library(StackAllocatorOverride)

    testBuilder_add_include(StackAllocatorOverrideL include)
    testBuilder_add_source(StackAllocatorOverrideL src/empty.cpp)
    testBuilder_add_library(StackAllocatorOverrideL StackAllocatorL)
    testBuilder_add_compile_option(StackAllocatorOverrideL "SHELL:-D SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW=1")
    testBuilder_build_shared_library(StackAllocatorOverrideL)

    testBuilder_add_source(stack_exe src/executable.cpp)
    testBuilder_add_library(stack_exe StackAllocatorOverride)
    testBuilder_build(stack_exe EXECUTABLES)
endif()