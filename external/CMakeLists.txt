add_subdirectory(boost)
add_subdirectory(mdspan)

if (MUNCH_BUILD_TESTS)
    add_subdirectory(googletest)
endif ()

if (MUNCH_BENCHMARK_COMPARE)
    add_subdirectory(compare)
endif ()
