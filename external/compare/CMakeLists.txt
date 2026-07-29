cmake_minimum_required(VERSION 3.20)
project(munch_compare)

include(FetchContent)

# The comparison engines are benchmarked as-is; their warnings are not ours to fix.
add_compile_options(-w)

set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE)

FetchContent_Declare(
        pcre2
        GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
        GIT_TAG pcre2-10.44
        GIT_SHALLOW TRUE
)

# Header-only and consumed by include path, like Boost.Regex below; skipping its build also keeps its
# unconditional install rules out of a munch install.
FetchContent_Declare(
        ctre
        GIT_REPOSITORY https://github.com/hanickadot/compile-time-regular-expressions.git
        GIT_TAG v3.9.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR unused
)

# Header-only and consumed by include path; munch's nearest relative in the comparison, a lexer built at run time
# from rules and compiled to a DFA.
FetchContent_Declare(
        lexertl
        GIT_REPOSITORY https://github.com/BenHanson/lexertl17.git
        GIT_TAG 652435f97f887df44bb6521deeff88dd8e949f6f
        SOURCE_SUBDIR unused
)

set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)

# RE2's install export references Abseil targets, which only exist in an export set when Abseil installs too.
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
        abseil
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20240722.0
        GIT_SHALLOW TRUE
)

set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        re2
        GIT_REPOSITORY https://github.com/google/re2.git
        GIT_TAG 2024-07-02
        GIT_SHALLOW TRUE
)

# Boost.Regex is consumed standalone (header-only, no other Boost libraries); its own CMake build expects the
# Boost superproject, so only the sources are fetched and an interface target carries the include path.
FetchContent_Declare(
        boost_regex
        GIT_REPOSITORY https://github.com/boostorg/regex.git
        GIT_TAG boost-1.86.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR unused
)

FetchContent_MakeAvailable(pcre2 ctre lexertl abseil re2 boost_regex)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME}
        SYSTEM INTERFACE
        ${boost_regex_SOURCE_DIR}/include
        ${ctre_SOURCE_DIR}/include
        ${lexertl_SOURCE_DIR}/include
)

target_compile_definitions(${PROJECT_NAME}
        INTERFACE
        BOOST_REGEX_STANDALONE
)

target_link_libraries(${PROJECT_NAME}
        INTERFACE
        pcre2-8-static
        re2::re2
)
