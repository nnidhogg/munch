cmake_minimum_required(VERSION 3.20.6)
project(munch_googletest)

option(USE_SYSTEM_GTEST "Use system-installed Googletest" OFF)

if (USE_SYSTEM_GTEST)
    find_package(GTest REQUIRED CONFIG)

    if (NOT GTest_FOUND)
        message(FATAL_ERROR "System Googletest not found!")
    endif ()

else ()
    include(FetchContent)

    # A test-only dependency; its install rules do not belong in a munch install.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

    # The archive-timestamp policy exists from CMake 3.24; the guarded form keeps the declared 3.20 minimum
    # honest, where the DOWNLOAD_EXTRACT_TIMESTAMP keyword would be misparsed as clone arguments instead.
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()

    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/release-1.12.1.zip
    )
    FetchContent_MakeAvailable(googletest)
endif ()
