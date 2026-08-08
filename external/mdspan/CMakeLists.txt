cmake_minimum_required(VERSION 3.20.6)
project(munch_mdspan)

add_library(${PROJECT_NAME} INTERFACE)

include(FetchContent)

# Only the sources are fetched: munch compiles against the include directory, and skipping the mdspan build keeps
# its unconditional install rules out of a munch install.
FetchContent_Declare(
        mdspan
        GIT_REPOSITORY https://github.com/kokkos/mdspan.git
        GIT_TAG 884f17a24301955d47cbb22318f06b8d8bee7ca3
        SOURCE_SUBDIR unused
)

FetchContent_MakeAvailable(mdspan)

target_include_directories(${PROJECT_NAME}
        INTERFACE
        ${mdspan_SOURCE_DIR}/include
)
