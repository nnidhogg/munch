#!/usr/bin/env bash
#
# Fails if a tracked Python file holds a line longer than 120 columns.
#
# The C++ and the docs are filled to 120 columns, and the probes, generators and paper checks written in Python keep the
# same width. A file kept byte for byte equal to an archived copy keeps that copy's lines instead, and one of them that
# holds a longer line is exempt by name below with the reason beside it.
#
# Usage: tools/check_python_lines.sh [files...]   (defaults to the tracked Python files)

set -euo pipefail

# Files kept as archived, which this check must not ask to change.
exempt=(
    # Byte for byte the copy the panic-mode report's campaign archive holds beside the data its statistics recompute
    # from, so its lines stay as archived.
    tools/probes/analysis/recovery-quality/test_analyze_r6.py
)

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(git ls-files '*.py')
fi

status=0

for file in "${files[@]}"; do
    [ -f "$file" ] || continue

    for known in "${exempt[@]}"; do
        [ "$file" = "$known" ] && continue 2
    done

    while IFS=: read -r line _; do
        echo "$file:$line is longer than 120 columns" >&2
        status=1
    done < <(LC_ALL=C.UTF-8 grep -nE '^.{121}' "$file" || true)
done

if [ "$status" -eq 0 ]; then
    echo "every tracked Python line is at most 120 columns"
fi

exit "$status"
