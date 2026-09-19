#!/bin/sh
# Runs munch_paper4_recompute's offline sections, the ones needing no external corpus, into a directory and pins
# the lines they decide: the UTF-8 shape's sync distance and the wide cutoff sweep's three counts, each a figure
# the certified-splitting paper's data directory carries and this probe recomputes from the library's decisions.
# Usage: paper4_offline.sh <executable> <output directory>
set -u
executable=$1
directory=$2
rm -rf "$directory" && mkdir -p "$directory" || exit 2
"$executable" "$directory" > /dev/null || { echo "the probe failed" >&2; exit 1; }
status=0
pin() {
    if ! grep -qxF -- "$2" "$directory/$1"; then
        echo "$1 lacks the line: $2" >&2
        status=1
    fi
}
pin splitting-stats.txt "utf8-shape | sync distance: 3"
pin wide-cutoff-sweep.txt "  triples checked: 45024"
pin wide-cutoff-sweep.txt "  triples with a counterexample (refuted): 25668"
pin wide-cutoff-sweep.txt "  refuted triples whose shortest counterexample attains the cutoff (tight): 754"
exit $status
