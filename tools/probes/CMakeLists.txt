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
