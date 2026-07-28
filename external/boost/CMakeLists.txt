cmake_minimum_required(VERSION 3.20)
project(munch_boost)

option(USE_SYSTEM_BOOST "Use system-installed Boost" OFF)

add_library(${PROJECT_NAME} INTERFACE)

if (USE_SYSTEM_BOOST)
    find_package(Boost REQUIRED COMPONENTS config describe mp11 container_hash)

    if (NOT Boost_FOUND)
        message(FATAL_ERROR "System Boost not found or missing required components.")
    endif ()

    target_include_directories(${PROJECT_NAME}
            INTERFACE
            ${Boost_INCLUDE_DIRS}
    )
else ()
    include(FetchContent)

    FetchContent_Declare(
            boost_config
            GIT_REPOSITORY https://github.com/boostorg/config.git
            GIT_TAG boost-1.90.0
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_Declare(
            boost_describe
            GIT_REPOSITORY https://github.com/boostorg/describe.git
            GIT_TAG boost-1.90.0
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_Declare(
            boost_mp11
            GIT_REPOSITORY https://github.com/boostorg/mp11.git
            GIT_TAG boost-1.90.0
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_Declare(
            boost_container_hash
            GIT_REPOSITORY https://github.com/boostorg/container_hash.git
            GIT_TAG boost-1.90.0
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(
            boost_config
            boost_describe
            boost_mp11
            boost_container_hash
    )

    target_include_directories(${PROJECT_NAME}
            INTERFACE
            ${boost_config_SOURCE_DIR}/include
            ${boost_describe_SOURCE_DIR}/include
            ${boost_mp11_SOURCE_DIR}/include
            ${boost_container_hash_SOURCE_DIR}/include
    )
endif ()
