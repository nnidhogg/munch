cmake_minimum_required(VERSION 3.20)
project(munch VERSION 0.3.1)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MUNCH_IS_TOP_LEVEL ON)
else ()
    set(MUNCH_IS_TOP_LEVEL OFF)
endif ()

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

include(GNUInstallDirs)

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
