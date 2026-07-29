cmake_minimum_required(VERSION 3.20)
project(munch)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# GCC-style flags; GCC on Linux is the only toolchain CI builds and tests against (see .github/workflows/ci.yml).
add_compile_options(-Wall -Werror -Wunused -Wextra -pthread)

add_compile_options(
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Release>:-O2>
)

option(MUNCH_BUILD_TESTS "Build tests for munch" ON)
if (MUNCH_BUILD_TESTS)
    enable_testing()
endif ()

option(MUNCH_BUILD_BENCHMARK "Build the throughput benchmark for munch" ON)

add_subdirectory(external)
add_subdirectory(libs)
add_subdirectory(tools)

add_library(${PROJECT_NAME}
        INTERFACE
)

target_link_libraries(${PROJECT_NAME}
        INTERFACE
        munch_core
        munch_tokenizer
)
