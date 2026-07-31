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
