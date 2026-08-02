#!/usr/bin/env bash
#
# Collects one benchmark run together with the environment it ran in, so the numbers can be cited later.
#
# Usage:  tools/benchmark/collect.sh [output directory] [sizes in MiB] [passes]
# Example: tools/benchmark/collect.sh /tmp/munch-run 1,16,128,512 15
#
# Produces, in the output directory:
#   environment.txt   what the machine and toolchain were
#   summary.txt       the human-readable scenario table
#   observations.csv  every timed pass of the scaling scenarios, not just best/median/worst; the construction,
#                     planning and thread-launch rows appear in summary.txt only
#
# Send back the tarball it produces. It deliberately avoids identifying the machine or its users: uname omits the
# hostname and the uptime line is printed without its count of logged-in users.
#
# Requirements:
#   cmake 3.20 or newer
#   a C++23 compiler: GCC 13+ or Clang 19+ (Clang 18 reports __cpp_concepts too low for libstdc++'s <expected>)
#   git and network access on the first configure: four header-only Boost libraries and mdspan are cloned from
#     GitHub at pinned revisions. -DUSE_SYSTEM_BOOST=ON skips the Boost clones but not mdspan, so a fully offline
#     configure is not supported.
#   pthreads, which is standard on Linux
#
# Newer toolchains are expected to work and are not specially handled: warnings are not treated as errors here, and
# a configure or build failure prints the log tail with the two retries that usually fix it.
#
#   Debian or Ubuntu:        sudo apt install -y build-essential cmake git
#   Fedora:                  sudo dnf install -y gcc-c++ cmake git
#   Arch:                    sudo pacman -S --needed base-devel cmake git

set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

command -v cmake >/dev/null || fail "cmake not found; install cmake 3.20 or newer"
command -v git   >/dev/null || fail "git not found; it is needed to fetch the pinned Boost headers"

cmake_version=$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
if [ "$(printf '%s\n3.20\n' "$cmake_version" | sort -V | head -1)" != "3.20" ]; then
    fail "cmake $cmake_version is too old; 3.20 or newer is required"
fi

# A C++23 feature the tree actually depends on, so this rejects a too-old compiler up front rather than mid-build.
probe=$(mktemp -d)
cat > "$probe/probe.cpp" <<'PROBE'
#include <expected>
#include <string>
int main() { std::expected<int, std::string> value{42}; return value.has_value() ? 0 : 1; }
PROBE
if ! "${CXX:-c++}" -std=c++23 "$probe/probe.cpp" -o "$probe/probe" 2>"$probe/err"; then
    echo "--- compiler error ---" >&2
    head -5 "$probe/err" >&2
    fail "${CXX:-c++} cannot compile C++23 with <expected>; use GCC 13+ or Clang 19+, or set CXX"
fi
rm -rf "$probe"

# uptime carries the load averages the non-quiescence discussion needs, on the same line as a count of logged-in
# users that nothing published rests on. Print the line without the count, so the machine's owner is not asked to
# trust a redaction after the fact.
load_line() {
    LC_ALL=C uptime | sed -E 's/,[[:space:]]*[0-9]+[[:space:]]+users?,/,/'
}

out=${1:-benchmark-run}
sizes=${2:-1,16,128,512}
passes=${3:-15}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

# Read the checkout's state before creating anything: the default output directory lives inside the repository, so
# creating it first would make a clean tree report itself dirty.
commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo 'not a git checkout')
dirty=$(test -n "$(git -C "$root" status --porcelain 2>/dev/null)" && echo yes || echo no)

mkdir -p "$out"
out=$(cd "$out" && pwd)

# The benchmark appends to the CSV, so a second run into the same directory silently interleaves two datasets that
# only the run column can separate, while summary.txt and environment.txt describe the last run alone. Refuse
# instead: a fresh directory per run keeps the three files describing the same measurement.
if [ -e "$out/observations.csv" ]; then
    fail "$out already holds observations.csv from an earlier run; use a new output directory"
fi

echo "munch benchmark collection"
echo "  repository: $root"
echo "  sizes:      $sizes MiB"
echo "  passes:     $passes"
echo "  output:     $out"
echo

# The measurement is only quotable if the machine it ran on is recorded with it.
{
    echo "collected:  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "commit:     $commit"
    echo "dirty:      $dirty"
    echo "sizes:      $sizes MiB"
    echo "passes:     $passes"
    echo
    echo "== cpu =="
    lscpu 2>/dev/null | grep -Ei '^(Architecture|Model name|CPU\(s\)|Thread|Core|Socket|Vendor|CPU max|CPU min|L1d|L2|L3|Hypervisor|Virtualization)' || true
    echo
    echo "== topology visible to the process =="
    echo "nproc:      $(nproc 2>/dev/null || echo unknown)"
    echo "governor:   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'not exposed')"
    echo "boost:      $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo 'not exposed')"
    echo
    echo "== memory =="
    free -h 2>/dev/null || true
    echo
    echo "== os =="
    # -a would print the hostname; the kernel, architecture and OS are what matter here.
    uname -srmo
    (. /etc/os-release 2>/dev/null && echo "distro:     $PRETTY_NAME") || true
    echo "container:  $(test -f /.dockerenv && echo yes || echo 'none detected')"
    echo "wsl:        $(grep -qi microsoft /proc/version 2>/dev/null && echo yes || echo no)"
    echo
    echo "== toolchain =="
    "${CXX:-c++}" --version 2>/dev/null | head -1
    cmake --version 2>/dev/null | head -1
    echo
    echo "== load at start =="
    load_line
} > "$out/environment.txt"

echo "recorded environment"

# Built outside the output directory on purpose: a build tree is hundreds of megabytes of binaries, fetched
# sources, and absolute paths, and the point of the output directory is that it can be sent as is.
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

# Warnings are not errors here on purpose. This script is meant to run on whatever compiler the machine has, and a
# newer one finding a new warning should not cost us the measurement; CI is where warnings are policed.
if ! cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DMUNCH_BUILD_BENCHMARK=ON -DMUNCH_BUILD_TESTS=OFF \
        -DMUNCH_WERROR=OFF ${MUNCH_CMAKE_ARGS:-} > "$out/configure.log" 2>&1; then
    echo "--- configure failed, last 20 lines ---" >&2
    tail -20 "$out/configure.log" >&2
    echo >&2
    echo "Two things commonly cause this:" >&2
    echo "  * CMake 4 rejecting an old cmake_minimum_required in a fetched dependency. Retry with:" >&2
    echo "      MUNCH_CMAKE_ARGS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5 $0 $*" >&2
    echo "  * no network access for the pinned dependencies. If Boost is installed, this skips its clones," >&2
    echo "    though mdspan is still fetched, so it does not make the configure work offline:" >&2
    echo "      MUNCH_CMAKE_ARGS=-DUSE_SYSTEM_BOOST=ON $0 $*" >&2
    fail "configure failed; $out/configure.log has the detail, and is worth sending back as is"
fi

if ! cmake --build "$build" -j "$(nproc 2>/dev/null || echo 4)" > "$out/build.log" 2>&1; then
    echo "--- build failed, last 20 lines ---" >&2
    tail -20 "$out/build.log" >&2
    fail "build failed; send back $out/build.log and environment.txt"
fi

echo "built"

# The benchmark validates the eight-chunk stream against the serial one before the scaling rows, so a wrong result
# fails loudly.
"$build/tools/benchmark/munch_benchmark" "$sizes" "$passes" "$out/observations.csv" | tee "$out/summary.txt"

{
    echo
    echo "== load at end =="
    load_line
} >> "$out/environment.txt"

# Packed as one file on purpose: the summary, the per-pass CSV, and the environment only mean anything together,
# and sending them loose invites a stray file from another run being read alongside them. The configure and build
# logs stay out of the archive: compiler diagnostics quote absolute source paths, which name the account that ran
# this, and the header promises the output identifies neither the machine nor its users. They are kept on disk
# beside the archive for the volunteer to inspect or send separately if a build problem needs them.
archive="$out.tar.gz"

# --owner/--group/--numeric-owner keep the volunteer's account out of the tar headers; the archive promises anonymity
# and the file contents are already scrubbed, so the metadata must be too.
tar czf "$archive" --owner=0 --group=0 --numeric-owner -C "$(dirname "$out")" \
    --exclude="$(basename "$out")/configure.log" --exclude="$(basename "$out")/build.log" \
    "$(basename "$out")"

echo
echo "done. Send back this one file: $archive"
ls -1 "$out" | sed 's/^/  /'
echo "  $(wc -l < "$out/observations.csv") CSV rows, $(wc -l < "$out/summary.txt") summary lines"
echo "  archive $(du -h "$archive" | cut -f1)"
