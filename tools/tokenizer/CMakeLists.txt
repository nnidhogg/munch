cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_tokenizer)

add_library(${PROJECT_NAME}
        src/error.cpp
        src/raw_string.cpp
        src/tokenizer.cpp
)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        include
)

target_link_libraries(${PROJECT_NAME}
        PUBLIC
        munch_core
)

if (MUNCH_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/raw_string_test.cpp
            tests/tokenizer_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            munch_core
            munch_regex
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()
