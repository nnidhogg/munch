# The input reader and the invariant check both harnesses share, its header under include/munch/tools/fuzz.
add_library(munch_fuzz
        src/reader.cpp
)

target_include_directories(munch_fuzz
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(munch_builder_fuzzer
        src/builder_fuzzer.cpp
)

target_link_libraries(munch_builder_fuzzer
        PRIVATE
        munch_core
        munch_fuzz
)

# fuzzer rather than the tree-wide fuzzer-no-link: this target carries libFuzzer's driver and its main().
target_link_options(munch_builder_fuzzer
        PRIVATE
        -fsanitize=fuzzer
)

add_executable(munch_raw_string_fuzzer
        src/raw_string_fuzzer.cpp
)

target_link_libraries(munch_raw_string_fuzzer
        PRIVATE
        munch_fuzz
        munch_tokenizer
)

target_link_options(munch_raw_string_fuzzer
        PRIVATE
        -fsanitize=fuzzer
)
