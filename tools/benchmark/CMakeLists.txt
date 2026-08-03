cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_benchmark)

add_executable(${PROJECT_NAME}
        src/harness.cpp
        src/main.cpp
)

add_dependencies(${PROJECT_NAME} munch_provenance)

target_include_directories(${PROJECT_NAME}
        PRIVATE
        include
        ${MUNCH_PROVENANCE_DIR}
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

add_dependencies(${PROJECT_NAME}_modes munch_provenance)

target_include_directories(${PROJECT_NAME}_modes
        PRIVATE
        include
        ${MUNCH_PROVENANCE_DIR}
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

    add_dependencies(${PROJECT_NAME}_compare munch_provenance)

    target_include_directories(${PROJECT_NAME}_compare
            PRIVATE
            include
            ${MUNCH_PROVENANCE_DIR}
    )

    target_link_libraries(${PROJECT_NAME}_compare
            PRIVATE
            munch_core
            munch_regex
            munch_compare
    )
endif ()
