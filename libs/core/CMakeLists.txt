cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_core)

add_library(${PROJECT_NAME}
        src/builder.cpp
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
        PRIVATE
        # Build-interface only: a header-only build dependency a consumer of the installed package never needs.
        $<BUILD_INTERFACE:munch_boost>
        PUBLIC
        munch_common
        munch_regex
        munch_dfa
        munch_nfa
)

if (MUNCH_INSTALL)
    install(TARGETS ${PROJECT_NAME} EXPORT munch_targets)

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (MUNCH_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/lexer_test.cpp
            tests/property_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            munch_dfa_tools
            munch_nfa_tools
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif ()
