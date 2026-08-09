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

# mdspan disables its C++23 bracket operator on Clang older than 17 and on exactly 17.0.0, working around an
# upstream parameter-pack bug fixed in LLVM 17.0.1. Apple renumbers Clang, so AppleClang 17.0.0, which is built on
# a later LLVM that carries the fix, collides with the excluded upstream release and loses the operator munch's
# table construction uses. The definition states, unconditionally and on every platform, what the documented
# compiler floor of GCC 13 and Clang 19 already guarantees: multidimensional subscript works, so the workaround
# gate is not consulted.
target_compile_definitions(${PROJECT_NAME}
        INTERFACE
        MDSPAN_USE_BRACKET_OPERATOR=1
)
