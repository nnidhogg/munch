cmake_minimum_required(VERSION 3.20.6)
cmake_policy(SET CMP0097 NEW)
project(munch_verifier)

add_library(${PROJECT_NAME}
        src/merge_tower.cpp
        src/ripple_radius.cpp
        src/token_automaton.cpp
        src/trie_chain.cpp
        src/verifier.cpp
        src/verifier_decisions.cpp
)

add_library(munch::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

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
        PUBLIC
        # The public headers include munch/dfa/dfa.hpp and armed_run() takes a dfa::Dfa, so consumers need the dfa
        # target too.
        munch_dfa
        PRIVATE
        # Build-interface only: a header-only build dependency a consumer of the installed package never needs.
        $<BUILD_INTERFACE:munch_boost>
)

if (MUNCH_INSTALL)
    install(TARGETS ${PROJECT_NAME} EXPORT munch_targets)

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (MUNCH_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/merge_tower_test.cpp
            tests/ripple_radius_test.cpp
            tests/token_automaton_test.cpp
            tests/trie_chain_test.cpp
            tests/verifier_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${MUNCH_SOURCE_DIR}")
endif ()
