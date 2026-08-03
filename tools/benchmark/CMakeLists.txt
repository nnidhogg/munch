cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_benchmark)

# The commit each benchmark reports, regenerated every build so it cannot go stale.
set(provenance_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)

set(provenance_header ${provenance_dir}/munch/tools/benchmark/provenance.hpp)

# Once at configure time so the header exists for the first compile, then again on every build to refresh it.
execute_process(COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/provenance.hpp.in
        -DOUTPUT=${provenance_header}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/provenance.cmake)

add_custom_target(${PROJECT_NAME}_provenance
        BYPRODUCTS ${provenance_header}
        COMMAND ${CMAKE_COMMAND}
                -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/provenance.hpp.in
                -DOUTPUT=${provenance_header}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/provenance.cmake
        COMMENT "Recording the commit the benchmarks are built from"
        VERBATIM)

add_executable(${PROJECT_NAME}
        src/harness.cpp
        src/main.cpp
)

add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_provenance)

target_include_directories(${PROJECT_NAME}
        PRIVATE
        include
        ${provenance_dir}
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        munch_core
        munch_regex
        munch_tokenizer
)

# The modal driver's scenario matrix. Its own target rather than rows in the main benchmark: it builds a corpus per
# axis point and is run when the modal lookup changes, not on every throughput check.
add_executable(${PROJECT_NAME}_modes
        src/modes.cpp
        src/harness.cpp
)

add_dependencies(${PROJECT_NAME}_modes ${PROJECT_NAME}_provenance)

target_include_directories(${PROJECT_NAME}_modes
        PRIVATE
        include
        ${provenance_dir}
)

target_link_libraries(${PROJECT_NAME}_modes
        PRIVATE
        munch_core
        munch_regex
)

if (MUNCH_BENCHMARK_COMPARE)
    add_executable(${PROJECT_NAME}_compare
            src/compare.cpp
            src/harness.cpp
    )

    add_dependencies(${PROJECT_NAME}_compare ${PROJECT_NAME}_provenance)

    target_include_directories(${PROJECT_NAME}_compare
            PRIVATE
            include
            ${provenance_dir}
    )

    target_link_libraries(${PROJECT_NAME}_compare
            PRIVATE
            munch_core
            munch_regex
            munch_compare
    )
endif ()
