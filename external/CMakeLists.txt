add_subdirectory(boost)
add_subdirectory(googletest)
add_subdirectory(mdspan)

if (MUNCH_BENCHMARK_COMPARE)
    add_subdirectory(compare)
endif ()
