cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(lexer_core)

add_library(${PROJECT_NAME}
        src/builder.cpp
)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        include
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        lexer_boost
        PUBLIC
        lexer_common
        lexer_regex
        lexer_dfa
        lexer_nfa
)

if (LEXER_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/lexer_test.cpp
            tests/property_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            lexer_dfa_tools
            lexer_nfa_tools
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()
