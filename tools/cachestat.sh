#!/usr/bin/env bash
# Cache-miss measurement per optimization commit (roadmap 3.4).
#
# Every number in the README's performance table is wall-clock; the claim behind
# them is "fewer cache misses". This script measures the misses themselves. For
# each commit it checks out a worktree, builds the Release benchmark, and runs
# BM_AddCancel/1000 and BM_AddMatch/1000 for a fixed iteration count under
# valgrind's cache simulator (callgrind --cache-sim=yes), collecting only while
# the benchmark function runs, and reports data-cache misses per operation.
#
# Simulation, deliberately: it is deterministic (the same binary gives the same
# count to the digit) and runs anywhere Linux does, including a CI VM whose
# hypervisor hides the PMU. What it cannot say is how long a miss took — no
# prefetcher, no out-of-order overlap, no coherence traffic — so it proves
# *fewer*, while the ns table proves *faster*. Hardware counters (`perf stat`)
# are tried first and reported when the machine exposes them (bare metal).
#
# Usage:  tools/cachestat.sh [--iterations N] [commit...]
#         default commits: the optimization commits from the README table + HEAD
# Output: a Markdown table on stdout (appended to $GITHUB_STEP_SUMMARY when set).
# Env:    CACHESTAT_SKIP_VALGRIND=1 runs the bench natively for a pipeline smoke test
#         (misses print as n/a) — the only way to exercise this on macOS.
set -euo pipefail

ITERATIONS=50000
COMMITS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --iterations)
        ITERATIONS="$2"
        shift 2
        ;;
    *)
        COMMITS+=("$1")
        shift
        ;;
    esac
done
if [[ ${#COMMITS[@]} -eq 0 ]]; then
    # 0.8 baseline, 1.1 ownership, 1.2 arena, 2.1 flat levels, 2.2 intrusive FIFO,
    # 3.1 sink, then whatever is checked out.
    COMMITS=(5dd83af 5b18318 f6bf84b 927f6c4 3a981ec a24ee1f HEAD)
fi

ROOT=$(git rev-parse --show-toplevel)
WORK=$(mktemp -d)
cleanup() {
    cd "$ROOT"
    for c in "${COMMITS[@]}"; do
        git worktree remove --force "$WORK/$c" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

SKIP_VALGRIND=${CACHESTAT_SKIP_VALGRIND:-0}
if [[ $SKIP_VALGRIND != 1 ]] && ! command -v valgrind >/dev/null; then
    echo "valgrind not found (set CACHESTAT_SKIP_VALGRIND=1 for a pipeline smoke test)" >&2
    exit 1
fi

# Fixed cache geometry so the simulation is identical on every host: a generic
# server core — 32 KB 8-way L1D, 64-byte lines, 8 MB 16-way last level.
CACHE=(--I1=32768,8,64 --D1=32768,8,64 --LL=8388608,16,64)

# perf hardware events are usable only when the PMU is exposed (not in GitHub's VMs).
PERF_OK=0
if command -v perf >/dev/null &&
    perf stat -e L1-dcache-load-misses true 2>&1 | grep -q "L1-dcache-load-misses" &&
    ! perf stat -e L1-dcache-load-misses true 2>&1 | grep -q "not supported"; then
    PERF_OK=1
fi

build() { # $1 = commit -> prints the bench binary path
    local dir="$WORK/$1"
    git -C "$ROOT" worktree add --detach --quiet "$dir" "$1"
    cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_BASE_DIR="$WORK/deps" >/dev/null
    cmake --build "$dir/build" --target orderbook_bench --parallel >/dev/null
    echo "$dir/build/OrderbookBench/orderbook_bench"
}

# Misses per iteration for one benchmark: "<L1D> <LL>" or "n/a n/a".
simulate() { # $1 = bench binary, $2 = benchmark name
    if [[ $SKIP_VALGRIND == 1 ]]; then
        "$1" --benchmark_filter="^$2/1000\$" --benchmark_min_time="${ITERATIONS}x" \
            --benchmark_min_warmup_time=0 >/dev/null 2>&1
        echo "n/a n/a"
        return
    fi
    local out="$WORK/callgrind.$2.out"
    # Collect only inside the benchmark function: the harness and the depth-1000
    # fill are excluded except for that fill, which is inside the function too
    # and worth ~2% of the iterations — small, constant across commits.
    valgrind --tool=callgrind --cache-sim=yes "${CACHE[@]}" --collect-atstart=no \
        "--toggle-collect=*$2*" --callgrind-out-file="$out" \
        "$1" --benchmark_filter="^$2/1000\$" --benchmark_min_time="${ITERATIONS}x" \
        --benchmark_min_warmup_time=0 >/dev/null 2>&1
    # The callgrind file names its event columns once and totals them once.
    awk -v n="$ITERATIONS" '
        /^events:/ { for (i = 2; i <= NF; i++) idx[$i] = i - 1 }
        /^summary:/ {
            for (i = 2; i <= NF; i++) v[i - 1] = $i
            printf "%.3f %.4f\n", (v[idx["D1mr"]] + v[idx["D1mw"]]) / n, (v[idx["DLmr"]] + v[idx["DLmw"]]) / n
        }' "$out"
}

# Hardware L1D load misses per iteration for the whole process (setup included), or n/a.
hardware() { # $1 = bench binary, $2 = benchmark name
    if [[ $PERF_OK != 1 ]]; then
        echo "n/a"
        return
    fi
    perf stat -x, -e L1-dcache-load-misses \
        "$1" --benchmark_filter="^$2/1000\$" --benchmark_min_time="${ITERATIONS}x" \
        --benchmark_min_warmup_time=0 2>&1 >/dev/null |
        awk -F, -v n="$ITERATIONS" '/L1-dcache-load-misses/ { printf "%.3f\n", $1 / n }'
}

{
    echo "| Commit | Change | L1D misses / add+cancel | L1D misses / match | LL misses / match | perf L1D / match (whole process) |"
    echo "|---|---|---|---|---|---|"
    for c in "${COMMITS[@]}"; do
        hash=$(git -C "$ROOT" rev-parse --short "$c")
        subject=$(git -C "$ROOT" log -1 --format=%s "$c")
        bench=$(build "$c")
        read -r addL1 _ <<<"$(simulate "$bench" BM_AddCancel)"
        read -r matchL1 matchLL <<<"$(simulate "$bench" BM_AddMatch)"
        hw=$(hardware "$bench" BM_AddMatch)
        echo "| \`$hash\` | $subject | $addL1 | $matchL1 | $matchLL | $hw |"
    done
    echo
    echo "callgrind --cache-sim=yes, ${CACHE[*]}, ${ITERATIONS} iterations per benchmark at depth 1000;"
    echo "misses counted only inside the benchmark function. perf column requires an exposed PMU."
} | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
