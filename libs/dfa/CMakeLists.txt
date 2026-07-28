cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(lexer_dfa)

add_library(${PROJECT_NAME}
        src/builder.cpp
        src/dfa.cpp
        src/label.cpp
        src/simulator.cpp
        src/token.cpp
)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        include
)

target_link_libraries(${PROJECT_NAME}
        PUBLIC
        lexer_common
        lexer_mdspan
        PRIVATE
        lexer_boost
)

add_library(${PROJECT_NAME}_tools
        tools/src/graphviz.cpp
)

target_include_directories(${PROJECT_NAME}_tools
        PUBLIC
        tools/include
)

target_link_libraries(${PROJECT_NAME}_tools
        ${PROJECT_NAME}
        lexer_common
)

if (LEXER_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/dfa_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            ${PROJECT_NAME}_tools
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    add_executable(${PROJECT_NAME}_tools_tests
            tools/tests/graphviz_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tools_tests
            PRIVATE
            ${PROJECT_NAME}
            ${PROJECT_NAME}_tools
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tools_tests COMMAND ${PROJECT_NAME}_tools_tests)

    target_compile_definitions(${PROJECT_NAME}_tools_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()
