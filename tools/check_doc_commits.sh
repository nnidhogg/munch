#!/usr/bin/env bash
#
# Fails if a commit the documentation names cannot be resolved from a fresh clone.
#
# A published figure is only checkable if the tree it was measured on can still be fetched. Squashing or force
# pushing rewrites SHAs, and a reference written before that points at a commit which still resolves locally, because
# a backup branch holds it, while resolving nowhere for a reader who clones. That is exactly how README figures came
# to name two dead commits on 2026-08-03, so this is checked rather than remembered.
#
# What counts as a reference: a backtick-quoted hexadecimal token of 7 to 40 characters carrying at least one letter.
# Reachability is from HEAD or from any tag, since a clone fetches both.
# The letter requirement keeps token counts and byte sizes out, which are decimal. A short SHA that happens to be all
# digits would be missed, so write eight characters or more when one turns up.
#
# Usage: tools/check_doc_commits.sh [files...]   (defaults to the tracked documentation)

set -euo pipefail

# Commits belonging to other repositories, which this one cannot resolve and must not try to.
foreign=(
    652435f                                  # lexertl17, the revision the engine comparison pins
    884f17a24301955d47cbb22318f06b8d8bee7ca3 # mdspan, the revision master pins
)

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# A clone fetches the branch and the tags, so either keeps a commit resolvable. The benchmark tags exist precisely to
# hold trees that master no longer contains, and the commits they preserve are cited on purpose.
reachable() {
    git merge-base --is-ancestor "$1" HEAD 2>/dev/null && return 0

    [ -n "$(git tag --contains "$1" 2>/dev/null)" ] && return 0

    return 1
}

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(git ls-files '*.md' '*.tex' 'CITATION.cff')
fi

status=0

for file in "${files[@]}"; do
    [ -f "$file" ] || continue

    while read -r sha; do
        for known in "${foreign[@]}"; do
            [ "$sha" = "$known" ] && continue 2
        done

        if ! git cat-file -e "$sha^{commit}" 2>/dev/null; then
            echo "$file: $sha is not a commit in this repository" >&2
            status=1
        elif ! reachable "$sha"; then
            echo "$file: $sha is reachable from no branch or tag, so a fresh clone cannot resolve it" >&2
            status=1
        fi
    done < <(grep -oE '`[0-9a-f]{7,40}`' "$file" | tr -d '`' | grep -E '[a-f]' | sort -u)
done

if [ "$status" -eq 0 ]; then
    echo "every commit named in the documentation is reachable from a branch or tag"
fi

exit "$status"
