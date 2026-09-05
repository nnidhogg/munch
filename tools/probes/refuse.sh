#!/bin/sh
# Runs a probe that must refuse: exit status exactly one and the named diagnostic on standard error.
# Usage: refuse.sh <executable> <diagnostic> <directory to create first, or empty> <arguments...>
# MUNCH_REFUSE_STDOUT, when set, is where the probe's standard output goes, /dev/full for a summary that
# cannot be written. The executable path is quoted throughout, so a path with spaces runs rather than failing
# with a shell status this script would not mistake for a refusal.
set -u
executable=$1
diagnostic=$2
directory=$3
shift 3
if [ -n "$directory" ]; then
    rm -rf "$directory" && mkdir -p "$directory" || exit 2
fi
errors=$(mktemp) || exit 2
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
