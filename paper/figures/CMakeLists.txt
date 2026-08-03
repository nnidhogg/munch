cmake_minimum_required(VERSION 3.20)
project(munch_paper_figures)

# The applicability table in paper/split-points.tex and docs/split_points.md is not documentation of a measurement,
# it is the output of this program, which asserts every published cell against what the shipped predicate answers.
# Running it under CTest is what makes "the table cannot silently drift" a property of the build rather than of
# someone remembering the manual command.
# harness.cpp comes along so the transcribed scaling grammar can be compared against the builder the evaluation
# actually runs, rather than against itself. It defines no main(), which is why the compare target links it the same way.
add_executable(munch_applicability
        applicability.cpp
        ${MUNCH_SOURCE_DIR}/tools/benchmark/src/harness.cpp
)

add_dependencies(munch_applicability munch_benchmark_provenance)

target_include_directories(munch_applicability
        PRIVATE
        ${MUNCH_SOURCE_DIR}/tools/benchmark/include
        ${MUNCH_BINARY_DIR}/tools/benchmark/generated
)

target_link_libraries(munch_applicability
        PRIVATE
        munch_core
        munch_tokenizer
)

add_test(NAME munch_applicability COMMAND munch_applicability)

# The three certificate figures are drawn from munch's own minimized automata by this program. Regenerating them
# into the build tree and comparing byte for byte against the committed DOT sources is what makes "the certificate
# figures are regenerated under CI" a property of the build rather than of someone remembering the manual command.
add_executable(munch_certificates certificates.cpp)

target_link_libraries(munch_certificates
        PRIVATE
        munch_core
        munch_dfa_tools
)

add_test(NAME munch_certificates COMMAND munch_certificates ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(munch_certificates PROPERTIES FIXTURES_SETUP certificate_figures)

foreach(figure certificate_sound certificate_nullable certificate_cyclic)
    add_test(NAME munch_certificates_${figure}
            COMMAND ${CMAKE_COMMAND} -E compare_files
            ${CMAKE_CURRENT_BINARY_DIR}/${figure}.dot ${CMAKE_CURRENT_SOURCE_DIR}/${figure}.dot)

    set_tests_properties(munch_certificates_${figure} PROPERTIES FIXTURES_REQUIRED certificate_figures)
endforeach()

# The composition comparison in paper/split-points.tex quotes how many states a prefix scan over line boundaries
# must carry. That is the whole of its argument, so it is computed from the automaton rather than counted by hand.
add_executable(munch_composition composition.cpp)

target_link_libraries(munch_composition
        PRIVATE
        munch_core
)

add_test(NAME munch_composition COMMAND munch_composition)

# The soundness and conservatism figures of paper/split-points.tex. The sweep is exhaustive over a small alphabet
# rather than sampled, because a sampled corpus reports a symbol as safe whenever it never exercised it.
add_executable(munch_validation validation.cpp)

target_link_libraries(munch_validation
        PRIVATE
        munch_core
)

add_test(NAME munch_validation COMMAND munch_validation)
