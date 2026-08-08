#!/usr/bin/env bash
# Pinned thread-count sweep for a bare-metal hybrid part: the measurement WSL2 cannot make, since a guest sees
# neither the real topology nor the frequency governor. Three affinity regimes answer where per-core efficiency
# falls: performance cores one thread per core, performance cores with their SMT siblings, and every CPU the
# machine has. Each regime runs the full benchmark under taskset with a widened chunk ladder, so the byte rows,
# the window rows, and the planning rows all land in one collection per regime.
#
# Usage: sweep.sh <output-dir> [sizes] [passes] [chunks]
#   sizes  comma-separated MiB list, default 16,128,512
#   passes interleaved rounds per scenario, default 15
#   chunks comma-separated chunk ladder, default 1,2,3,4,6,8,12,16
#
# Refuses to run under WSL and refuses a dirty tree: a figure is quotable only beside a clean commit on the
# machine it names.

set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

out=${1:?usage: sweep.sh <output-dir> [sizes] [passes] [chunks]}
sizes=${2:-16,128,512}
passes=${3:-15}
chunks=${4:-1,2,3,4,6,8,12,16}

grep -qi microsoft /proc/version && fail "this is WSL; the sweep exists to measure the bare-metal host"

repo=$(cd "$(dirname "$0")/../.." && pwd)

git -C "$repo" diff --quiet && git -C "$repo" diff --cached --quiet \
    || fail "the tree has uncommitted changes; commit first, a dirty figure is not quotable"

commit=$(git -C "$repo" rev-parse --short HEAD)

mkdir -p "$out"

# The topology, from the kernel's own hybrid interface; a homogeneous part gets the all-CPUs regime only.
if [ -r /sys/devices/cpu_core/cpus ]; then
    pcores_all=$(cat /sys/devices/cpu_core/cpus)
    ecores_all=$(cat /sys/devices/cpu_atom/cpus 2>/dev/null || echo "")

    # One thread per physical core: the first sibling listed for every CPU, deduplicated, then intersected
    # with the performance-core range so only performance cores remain.
    pcores_solo=$(for siblings in /sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list; do
        [ -r "$siblings" ] && cut -d, -f1 "$siblings" | cut -d- -f1
    done | sort -un | paste -sd,)

    pcores_solo=$(python3 - "$pcores_all" "$pcores_solo" <<'PY'
import sys
def expand(spec):
    cpus=set()
    for part in spec.split(','):
        if '-' in part:
            lo,hi=part.split('-'); cpus.update(range(int(lo),int(hi)+1))
        elif part:
            cpus.add(int(part))
    return cpus
p=expand(sys.argv[1]); solo=expand(sys.argv[2])
print(','.join(str(c) for c in sorted(p & solo)))
PY
)

    regimes=("pcore:$pcores_solo" "psmt:$pcores_all")
    [ -n "$ecores_all" ] && regimes+=("all:$pcores_all,$ecores_all")
else
    echo "no hybrid topology exposed; running the all-CPUs regime only"
    regimes=("all:0-$(($(nproc) - 1))")
fi

# The governor, best effort: without it the sweep still runs, and environment.txt records what actually held.
if command -v cpupower >/dev/null; then
    sudo cpupower frequency-set -g performance >/dev/null 2>&1 \
        || echo "could not set the performance governor; recording whatever is active"
fi

build="$out/build"

cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DMUNCH_BUILD_TESTS=OFF > "$out/configure.log" 2>&1 \
    || fail "configure failed; see $out/configure.log"

cmake --build "$build" -j "$(nproc)" --target munch_benchmark > "$out/build.log" 2>&1 \
    || fail "build failed; see $out/build.log"

{
    echo "commit      $commit"
    echo "collected   $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "sizes       $sizes MiB, $passes passes, chunks $chunks"
    echo "regimes     ${regimes[*]}"
    echo "kernel      $(uname -srm)"
    echo "governor    $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo
    lscpu
} > "$out/environment.txt"

for regime in "${regimes[@]}"; do
    name=${regime%%:*}
    mask=${regime#*:}

    echo "regime $name on CPUs $mask"

    taskset -c "$mask" "$build/tools/benchmark/munch_benchmark" \
        "$sizes" "$passes" "$out/observations-$name.csv" "$chunks" \
        > "$out/summary-$name.txt" 2>&1 \
        || fail "regime $name failed; see $out/summary-$name.txt"
done

echo "sweep complete: $out holds environment.txt and per-regime summaries and observations"
