# The installed munch package is otherwise self-contained: its build-time header-only dependencies (Boost
# utilities, mdspan) never appear in its public headers. Threads is the one public dependency, spawned by the
# parallel tokenization entry point.
include(CMakeFindDependencyMacro)
find_dependency(Threads)

include("${CMAKE_CURRENT_LIST_DIR}/munch-targets.cmake")
