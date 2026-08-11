cmake_minimum_required(VERSION 3.20.6)
project(munch_probes)

# Programs backing claims made outside this repository, in the same spirit as paper/figures: the numbers are asserted
# against what the shipped predicates answer, so a note citing them cannot drift from the library silently.
add_executable(munch_mode_cut_gate src/mode_cut_gate.cpp)

target_link_libraries(munch_mode_cut_gate
        PRIVATE
        munch_core
)

# The window search reuses the applicability figures' grammars, so a row measured here is the same row the table
# publishes rather than a transcription of it.
add_executable(munch_window_gate src/window_gate.cpp)

target_include_directories(munch_window_gate
        PRIVATE
        ${MUNCH_SOURCE_DIR}/paper/figures
)

target_link_libraries(munch_window_gate
        PRIVATE
        munch_core
)

if (MUNCH_BUILD_TESTS)
    add_test(NAME munch_mode_cut_gate COMMAND munch_mode_cut_gate)

    add_test(NAME munch_window_gate COMMAND munch_window_gate)
endif ()

# Quality of certified error recovery against the classical panic-mode conventions, on the study's grammar rows
# under a deterministic corruption model. Runs as a test because it carries the pristine oracle and the theorem
# transfer as hard assertions; the campaign form passes larger figures and archives the CSV.
add_executable(munch_recovery_quality src/recovery_quality.cpp)

target_include_directories(munch_recovery_quality
        PRIVATE
        ${MUNCH_SOURCE_DIR}/paper/figures
)

target_link_libraries(munch_recovery_quality
        PRIVATE
        munch_core
)

if (MUNCH_BUILD_TESTS)
    add_test(NAME munch_recovery_quality COMMAND munch_recovery_quality)
endif ()

# Occurrence of certified positions on corpora, the empirical question both reports name and neither answers.
# Runs as a test because the grammar-level facts (no exact bytes on the published rows, the whitespace-anchored
# two-byte census, the structural poison-byte windows) and the generated-corpus plan-and-splice equality are hard
# assertions; the campaign form takes a corpus directory and archives the CSV under the collection ritual.
add_executable(munch_corpus_windows src/corpus_windows.cpp)

target_include_directories(munch_corpus_windows
        PRIVATE
        ${MUNCH_SOURCE_DIR}/paper/figures
)

target_link_libraries(munch_corpus_windows
        PRIVATE
        munch_core
)

if (MUNCH_BUILD_TESTS)
    add_test(NAME munch_corpus_windows COMMAND munch_corpus_windows)
endif ()

# Dev-grade preview of the split-windows benchmark campaign: plans cuts at window-recovered origins, asserts
# stream equality against the serial scan, and prints unarchived throughput. The equality assertions are why it
# runs as a test; no printed figure may be quoted without the collect.sh ritual.
add_executable(munch_window_bench src/window_bench.cpp)

add_dependencies(munch_window_bench munch_provenance)

target_include_directories(munch_window_bench
        PRIVATE
        ${MUNCH_SOURCE_DIR}/paper/figures
        ${MUNCH_PROVENANCE_DIR}
)

target_link_libraries(munch_window_bench
        PRIVATE
        munch_core
)

if (MUNCH_BUILD_TESTS)
    add_test(NAME munch_window_bench COMMAND munch_window_bench)
endif ()
