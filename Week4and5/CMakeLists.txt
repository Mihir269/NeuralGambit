cmake_minimum_required(VERSION 3.14)
project(MiniAlphaBeta CXX)

# ---------------------------------------------------------------------------
# Build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to a Release build (with -O3) if the user didn't pick one.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# ---------------------------------------------------------------------------
# Dependency: Disservin's header-only chess-library (just chess.hpp).
#
# Option A (default): let CMake fetch the single header for you.
# Option B: drop chess.hpp next to this file yourself and CMake will use it.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chess.hpp")
    message(STATUS "chess.hpp not found locally - downloading it...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/Disservin/chess-library/master/include/chess.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/chess.hpp"
        SHOW_PROGRESS
        STATUS dl_status)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        message(FATAL_ERROR
            "Could not download chess.hpp. Please download it manually from "
            "https://github.com/Disservin/chess-library and place it beside CMakeLists.txt.")
    endif()
endif()

# ---------------------------------------------------------------------------
# The engine itself
# ---------------------------------------------------------------------------
add_executable(engine main.cpp)
target_include_directories(engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# A threads library is required for std::thread (the search runs off-thread so
# the UCI loop can still receive "stop").
find_package(Threads REQUIRED)
target_link_libraries(engine PRIVATE Threads::Threads)

# -march=native squeezes out extra speed on the machine you compile on. Comment
# this out if you plan to copy the binary to a different CPU.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
if(HAS_MARCH_NATIVE)
    target_compile_options(engine PRIVATE -march=native)
endif()
