cmake_minimum_required(VERSION 3.20)
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
