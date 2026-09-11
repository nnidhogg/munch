cmake_minimum_required(VERSION 3.20.6)
cmake_policy(SET CMP0097 NEW)
project(munch_audit)

add_library(${PROJECT_NAME}
        src/lexer_spec.cpp
        src/price.cpp
        src/read_flex.cpp
        src/read_re2c.cpp
        src/report.cpp
        src/token_set.cpp
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
        PUBLIC
        munch_core
        munch_regex
)

# The command: munch-audit FILE..., the readers and the report behind one program.
add_executable(${PROJECT_NAME}_cli src/main.cpp)

set_target_properties(${PROJECT_NAME}_cli PROPERTIES OUTPUT_NAME munch-audit)

target_link_libraries(${PROJECT_NAME}_cli
        PRIVATE
        ${PROJECT_NAME}
)

if (MUNCH_INSTALL)
    install(TARGETS ${PROJECT_NAME} ${PROJECT_NAME}_cli EXPORT munch_targets)

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif ()

if (MUNCH_BUILD_TESTS)
    add_executable(${PROJECT_NAME}_tests
            tests/read_flex_test.cpp
            tests/read_re2c_test.cpp
            tests/report_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    target_compile_definitions(${PROJECT_NAME}_tests PRIVATE SOURCE_DIR="${MUNCH_SOURCE_DIR}")
endif ()
