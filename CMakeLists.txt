cmake_minimum_required(VERSION 3.22)

project(Friction VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FRICTION_BUILD_PLUGIN "Build the JUCE plugin (VST3 + Standalone)" ON)
option(FRICTION_BUILD_TESTS  "Build the offline verification harness" ON)
set(FRICTION_JUCE_TAG "8.0.15" CACHE STRING "Pinned JUCE release tag")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    # static CRT: the plugin then needs no VC++ redistributable on the user's machine
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/MP /utf-8)
endif()

# JUCE-independent DSP core (always built)
add_subdirectory(src/core)

if(FRICTION_BUILD_PLUGIN)
    include(FetchContent)
    # Set FETCHCONTENT_SOURCE_DIR_JUCE to use a local checkout of the same tag.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${FRICTION_JUCE_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(src/plugin)
endif()

if(FRICTION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
