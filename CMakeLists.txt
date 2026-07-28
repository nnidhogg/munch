cmake_minimum_required(VERSION 3.20)
project(munch)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler-specific warning flags
if (MSVC)
    add_compile_options(/W4 /WX)
else ()
    add_compile_options(-Wall -Werror -Wunused -Wextra -pthread)
endif ()

add_compile_options(
        $<$<CONFIG:Debug>:$<IF:$<CXX_COMPILER_ID:MSVC>,/Od,-g>>
        $<$<CONFIG:Release>:$<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>>
)

option(MUNCH_BUILD_TESTS "Build tests for lexer" ON)
if (MUNCH_BUILD_TESTS)
    enable_testing()
endif ()

add_subdirectory(external)
add_subdirectory(libs)
add_subdirectory(tools)

add_library(${PROJECT_NAME}
        INTERFACE
)

target_link_libraries(${PROJECT_NAME}
        INTERFACE
        munch_core
        munch_tokenizer
)
