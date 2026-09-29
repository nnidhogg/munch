cmake_minimum_required(VERSION 3.20.6)
cmake_policy(SET CMP0097 NEW)
project(munch_audit)

add_library(${PROJECT_NAME}
        src/action_return.cpp
        src/antlr_alphabet.cpp
        src/antlr_checks.cpp
        src/antlr_commands.cpp
        src/antlr_cursor.cpp
        src/antlr_expression.cpp
        src/antlr_grammar.cpp
        src/antlr_members.cpp
        src/antlr_rule.cpp
        src/c_tokens.cpp
        src/command_line.cpp
        src/cursor.cpp
        src/directives.cpp
        src/expression.cpp
        src/file_kind.cpp
        src/flex_actions.cpp
        src/flex_lines.cpp
        src/flex_options.cpp
        src/flex_pattern.cpp
        src/flex_sections.cpp
        src/lexer_spec.cpp
        src/logos_attributes.cpp
        src/logos_callback.cpp
        src/logos_pattern.cpp
        src/logos_refusals.cpp
        src/logos_regex.cpp
        src/pattern_shape.cpp
        src/price.cpp
        src/re2c_actions.cpp
        src/re2c_block.cpp
        src/re2c_classes.cpp
        src/re2c_configuration.cpp
        src/re2c_regex.cpp
        src/read_antlr.cpp
        src/read_flex.cpp
        src/read_logos.cpp
        src/read_re2c.cpp
        src/report.cpp
        src/report_render.cpp
        src/rust_cursor.cpp
        src/rust_items.cpp
        src/rust_names.cpp
        src/scalar_set.cpp
        src/supply.cpp
        src/token_set.cpp
        src/word_kinds.cpp
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
            tests/cursor_test.cpp
            tests/expression_test.cpp
            tests/main_test.cpp
            tests/read_antlr_test.cpp
            tests/read_flex_test.cpp
            tests/read_logos_test.cpp
            tests/read_re2c_test.cpp
            tests/report_test.cpp
            tests/supply_test.cpp
            tests/twins_test.cpp
    )

    target_link_libraries(${PROJECT_NAME}_tests
            PRIVATE
            ${PROJECT_NAME}
            gtest_main
    )

    add_test(NAME ${PROJECT_NAME}_tests COMMAND ${PROJECT_NAME}_tests)

    # The command's own tests run the program built beside them.
    add_dependencies(${PROJECT_NAME}_tests ${PROJECT_NAME}_cli)

    target_compile_definitions(${PROJECT_NAME}_tests
            PRIVATE
            SOURCE_DIR="${MUNCH_SOURCE_DIR}"
            MUNCH_AUDIT="$<TARGET_FILE:${PROJECT_NAME}_cli>"
    )
endif ()
