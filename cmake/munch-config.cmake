# The installed munch package is self-contained: its build-time header-only dependencies (Boost utilities, mdspan)
# never appear in its public headers, so there are no find_dependency() calls to make.
include("${CMAKE_CURRENT_LIST_DIR}/munch-targets.cmake")
