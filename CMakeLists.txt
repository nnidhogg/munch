cmake_minimum_required(VERSION 3.20)
project(munch VERSION 0.3.1)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# GCC-style flags; CI builds and tests against GCC and Clang on Linux (see .github/workflows/ci.yml).
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

option(MUNCH_BENCHMARK_COMPARE "Build the benchmark comparing munch against other engines" OFF)

include(GNUInstallDirs)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MUNCH_IS_TOP_LEVEL ON)
else ()
    set(MUNCH_IS_TOP_LEVEL OFF)
endif ()

option(MUNCH_INSTALL "Generate install rules for munch" ${MUNCH_IS_TOP_LEVEL})

add_subdirectory(external)
add_subdirectory(libs)
add_subdirectory(tools)

add_library(${PROJECT_NAME}
        INTERFACE
)

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME}
        INTERFACE
        munch_core
        munch_tokenizer
)

if (MUNCH_INSTALL)
    install(TARGETS ${PROJECT_NAME} EXPORT munch_targets)

    install(EXPORT munch_targets
            FILE munch-targets.cmake
            NAMESPACE munch::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/munch
    )

    include(CMakePackageConfigHelpers)

    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/munch-config-version.cmake
            COMPATIBILITY SameMajorVersion
    )

    install(FILES
            cmake/munch-config.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/munch-config-version.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/munch
    )
endif ()
