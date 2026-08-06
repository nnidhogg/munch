if (MUNCH_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif ()

if (MUNCH_BUILD_FUZZER)
    add_subdirectory(fuzz)
endif ()

# Probes back claims made outside the repository; a consumer building munch as a dependency needs neither them
# nor their runtime, so they follow the same contract as the benchmarks and tests.
if (MUNCH_BUILD_TESTS OR MUNCH_BUILD_BENCHMARK)
    add_subdirectory(probes)
endif ()

add_subdirectory(tokenizer)
