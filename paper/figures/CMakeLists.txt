cmake_minimum_required(VERSION 3.20)
project(munch_paper_figures)

# The applicability table in paper/split-points.tex and docs/split_points.md is not documentation of a measurement,
# it is the output of this program, which asserts every published cell against what the shipped predicate answers.
# Running it under CTest is what makes "the table cannot silently drift" a property of the build rather than of
# someone remembering the manual command.
add_executable(munch_applicability applicability.cpp)

target_link_libraries(munch_applicability
        PRIVATE
        munch_core
)

add_test(NAME munch_applicability COMMAND munch_applicability)

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
