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
