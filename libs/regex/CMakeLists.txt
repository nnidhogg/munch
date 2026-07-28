cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_regex)

add_library(${PROJECT_NAME}
        src/any_of.cpp
        src/choice.cpp
        src/concat.cpp
        src/patterns.cpp
        src/regex.cpp
        src/repeat.cpp
        src/set.cpp
        src/text.cpp
        src/utf8.cpp
)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        include
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        munch_nfa
)

if (MUNCH_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/any_of_test.cpp
            tests/choice_test.cpp
            tests/concat_test.cpp
            tests/patterns_test.cpp
            tests/repeat_test.cpp
            tests/set_test.cpp
            tests/text_test.cpp
            tests/utf8_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            munch_nfa_tools
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()
