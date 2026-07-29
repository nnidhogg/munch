cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0097 NEW)
project(munch_benchmark)

add_executable(${PROJECT_NAME}
        src/harness.cpp
        src/main.cpp
)

target_include_directories(${PROJECT_NAME}
        PRIVATE
        include
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        munch_core
        munch_regex
        munch_tokenizer
)

if (MUNCH_BENCHMARK_COMPARE)
    add_executable(${PROJECT_NAME}_compare
            src/compare.cpp
            src/harness.cpp
    )

    target_include_directories(${PROJECT_NAME}_compare
            PRIVATE
            include
    )

    target_link_libraries(${PROJECT_NAME}_compare
            PRIVATE
            munch_core
            munch_regex
            munch_compare
    )
endif ()
