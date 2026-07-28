cmake_minimum_required(VERSION 3.20)
project(munch_mdspan)

add_library(${PROJECT_NAME} INTERFACE)

include(FetchContent)

FetchContent_Declare(
        mdspan
        GIT_REPOSITORY https://github.com/kokkos/mdspan.git
        GIT_TAG stable
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(mdspan)

target_include_directories(${PROJECT_NAME}
        INTERFACE
        ${mdspan_SOURCE_DIR}/include
)
