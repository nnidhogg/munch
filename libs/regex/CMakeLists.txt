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
        src/unicode.cpp
        src/utf8.cpp
)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(${PROJECT_NAME}
        PUBLIC
        cxx_std_23
)

target_link_libraries(${PROJECT_NAME}
        # Public because regex.hpp includes munch/nfa/builder.hpp and to_nfa() returns nfa::Builder by value, so a
        # consumer of this target needs the nfa headers to compile against it. In-tree builds hide that today by
        # reaching munch_nfa through munch_core's public edge; an installed consumer linking only munch_regex does not.
        PUBLIC
        munch_nfa
)

if (MUNCH_INSTALL)
    install(TARGETS ${PROJECT_NAME} EXPORT munch_targets)

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (MUNCH_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/any_of_test.cpp
            tests/indirect_test.cpp
            tests/choice_test.cpp
            tests/concat_test.cpp
            tests/patterns_test.cpp
            tests/repeat_test.cpp
            tests/set_test.cpp
            tests/text_test.cpp
        tests/unicode_test.cpp
            tests/utf8_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            munch_nfa_tools
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${MUNCH_SOURCE_DIR}")
endif ()
