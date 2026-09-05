#!/bin/sh
# Runs a probe that must refuse: exit status exactly one and the named diagnostic on standard error.
# Usage: refuse.sh <executable> <diagnostic> <directory to create first, or empty> <arguments...>
# MUNCH_REFUSE_STDOUT, when set, is where the probe's standard output goes, /dev/full for a summary that
# cannot be written; MUNCH_REFUSE_UNBUFFERED, when set, asks the probe to unbuffer its standard output
# through its MUNCH_UNBUFFERED_STDOUT hook, so that every summary write fails as it happens and only a
# harness that reads the stream's error indicator refuses, the final flush of an empty buffer succeeding.
# The hook lives in the probe rather than in a preloading tool, which the sanitizer builds refuse. The
# executable path is quoted throughout, so a path with spaces runs rather than failing with a shell status
# this script would not mistake for a refusal.
set -u
executable=$1
diagnostic=$2
directory=$3
shift 3
if [ -n "$directory" ]; then
    rm -rf "$directory" && mkdir -p "$directory" || exit 2
fi
# The diagnostic file goes to the temporary directory when there is one and beside the working directory
# otherwise, so a machine without /tmp still runs the refusals rather than failing their setup.
errors=$(mktemp 2> /dev/null) || errors="$PWD/refuse-$$.stderr"
if [ -n "${MUNCH_REFUSE_UNBUFFERED:-}" ]; then
    MUNCH_UNBUFFERED_STDOUT=1
    export MUNCH_UNBUFFERED_STDOUT
fi
set -- "$executable" "$@"
if [ -n "${MUNCH_REFUSE_STDOUT:-}" ]; then
    "$@" > "$MUNCH_REFUSE_STDOUT" 2> "$errors"
else
    "$@" > /dev/null 2> "$errors"
fi
status=$?
if [ "$status" -ne 1 ]; then
    echo "expected exit status 1, got $status" >&2
    cat "$errors" >&2
    rm -f "$errors"
    exit 1
fi
if ! grep -q -- "$diagnostic" "$errors"; then
    echo "diagnostic '$diagnostic' missing from standard error:" >&2
    cat "$errors" >&2
    rm -f "$errors"
    exit 1
fi
rm -f "$errors"
