# Run at build time, not configure time: a configure-time capture names the tree the build directory was created
# from and would keep naming it after every later commit.

execute_process(
        COMMAND git -C "${SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

execute_process(
        COMMAND git -C "${SOURCE_DIR}" status --porcelain
        OUTPUT_VARIABLE changes
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

if (NOT commit)
    set(commit "unknown")
endif ()

if (changes)
    set(dirty "true")
else ()
    set(dirty "false")
endif ()

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
