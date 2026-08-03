if (MUNCH_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif ()

if (MUNCH_BUILD_FUZZER)
    add_subdirectory(fuzz)
endif ()

add_subdirectory(probes)

add_subdirectory(tokenizer)
