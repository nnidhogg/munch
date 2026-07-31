add_executable(munch_builder_fuzzer
        src/builder_fuzzer.cpp
)

target_link_libraries(munch_builder_fuzzer
        PRIVATE
        munch_core
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
        munch_tokenizer
)

target_link_options(munch_raw_string_fuzzer
        PRIVATE
        -fsanitize=fuzzer
)
