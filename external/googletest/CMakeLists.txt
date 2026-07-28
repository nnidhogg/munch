cmake_minimum_required(VERSION 3.20)
project(munch_googletest)

option(USE_SYSTEM_GTEST "Use system-installed Googletest" OFF)

if (USE_SYSTEM_GTEST)
    find_package(GTest REQUIRED CONFIG)

    if (NOT GTest_FOUND)
        message(FATAL_ERROR "System Googletest not found!")
    endif ()

else ()
    include(FetchContent)

    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/release-1.12.1.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif ()
