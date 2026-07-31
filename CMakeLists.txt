cmake_minimum_required(VERSION 3.20)
project(munch VERSION 1.1.1)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MUNCH_IS_TOP_LEVEL ON)
else ()
    set(MUNCH_IS_TOP_LEVEL OFF)
endif ()

# munch's own root for test data paths; CMAKE_SOURCE_DIR names the embedding project's root under add_subdirectory().
set(MUNCH_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Defaulting the build type is a top-level convenience only; a parent project's choice is its own.
if (MUNCH_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# GCC-style flags; CI builds and tests against GCC and Clang on Linux (see .github/workflows/ci.yml). Threading
# is carried by Threads::Threads on the targets that need it, not by a global flag.
add_compile_options(-Wall -Wunused -Wextra)

# Warnings are errors for munch's own development; a consumer's newer compiler must not break their build over a
# warning munch has not seen yet.
option(MUNCH_WERROR "Treat warnings as errors" ${MUNCH_IS_TOP_LEVEL})
if (MUNCH_WERROR)
    add_compile_options(-Werror)
endif ()

add_compile_options(
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Release>:-O2>
)

# Developer-only targets stay out of consumer builds: when munch is a subdirectory of a larger project, tests and
# the benchmark build only on request.
option(MUNCH_BUILD_TESTS "Build tests for munch" ${MUNCH_IS_TOP_LEVEL})
if (MUNCH_BUILD_TESTS)
    enable_testing()
endif ()

option(MUNCH_BUILD_BENCHMARK "Build the throughput benchmark for munch" ${MUNCH_IS_TOP_LEVEL})

option(MUNCH_BENCHMARK_COMPARE "Build the benchmark comparing munch against other engines" OFF)

# Requires Clang, which carries libFuzzer. The whole tree is instrumented, not just the harness: the fuzzer's
# coverage feedback and the sanitizers must see library code, so a fuzzer build is its own dedicated build tree.
option(MUNCH_BUILD_FUZZER "Build the libFuzzer harness for munch (requires Clang)" OFF)
if (MUNCH_BUILD_FUZZER)
    add_compile_options(-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif ()

include(GNUInstallDirs)

option(MUNCH_INSTALL "Generate install rules for munch" ${MUNCH_IS_TOP_LEVEL})

add_subdirectory(external)
add_subdirectory(libs)
add_subdirectory(tools)

# The paper's applicability table asserts itself against the shipped predicate, so it is a test rather than a figure.
if (MUNCH_BUILD_TESTS)
    add_subdirectory(paper/figures)
endif ()

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

    # The Unicode License v3 requires its complete notice to accompany copies of the generated identifier tables.
    install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/munch)

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
