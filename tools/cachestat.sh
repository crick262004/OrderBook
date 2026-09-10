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
#         Any build, valgrind or parse failure aborts with that tool's log: an
#         empty cell is never silently produced.
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

die() { # $1 = message, $2 = log file to show
    echo "cachestat: $1" >&2
    if [[ -n ${2:-} && -f $2 ]]; then
        echo "--- $2 (tail) ---" >&2
        tail -n 40 "$2" >&2
    fi
    exit 1
}

SKIP_VALGRIND=${CACHESTAT_SKIP_VALGRIND:-0}
if [[ $SKIP_VALGRIND != 1 ]]; then
    command -v valgrind >/dev/null || die "valgrind not found (CACHESTAT_SKIP_VALGRIND=1 for a pipeline smoke test)"
    valgrind --version >&2
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

bench_args() { # $1 = benchmark name
    echo "--benchmark_filter=^$1/1000\$ --benchmark_min_time=${ITERATIONS}x --benchmark_min_warmup_time=0"
}

build() { # $1 = commit -> prints the bench binary path; dies with the build log on failure
    local dir="$WORK/$1" log="$WORK/build.$1.log"
    git -C "$ROOT" worktree add --detach --quiet "$dir" "$1" || die "worktree for $1 failed"
    cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release \
        -DFETCHCONTENT_BASE_DIR="$WORK/deps" >"$log" 2>&1 || die "configure of $1 failed" "$log"
    cmake --build "$dir/build" --target orderbook_bench --parallel >>"$log" 2>&1 || die "build of $1 failed" "$log"
    local bin="$dir/build/OrderbookBench/orderbook_bench"
    [[ -x $bin ]] || die "no bench binary at $bin after building $1" "$log"
    echo "$bin"
}

# Misses per iteration for one benchmark: "<L1D> <LL>" (or "n/a n/a" in smoke mode).
simulate() { # $1 = bench binary, $2 = benchmark name
    local log="$WORK/run.$2.log" out="$WORK/callgrind.$2.out"
    # shellcheck disable=SC2046
    if [[ $SKIP_VALGRIND == 1 ]]; then
        "$1" $(bench_args "$2") >"$log" 2>&1 || die "bench $2 failed" "$log"
        echo "n/a n/a"
        return
    fi
    # Collect only inside the benchmark function: the harness is excluded. The
    # depth-1000 fill is inside the function too — ~2% of the iterations, the
    # same on every commit.
    valgrind --tool=callgrind --cache-sim=yes "${CACHE[@]}" --collect-atstart=no \
        "--toggle-collect=*$2*" --callgrind-out-file="$out" \
        "$1" $(bench_args "$2") >"$log" 2>&1 || die "valgrind on $2 failed" "$log"
    [[ -s $out ]] || die "valgrind wrote no callgrind file for $2" "$log"
    # The callgrind file names its event columns once ("events:") and totals them
    # once ("summary:" in the header or "totals:" at the end).
    local parsed
    parsed=$(awk -v n="$ITERATIONS" '
        /^events:/ { for (i = 2; i <= NF; i++) idx[$i] = i - 1 }
        /^(summary|totals):/ {
            for (i = 2; i <= NF; i++) v[i - 1] = $i
            printf "%.3f %.4f\n", (v[idx["D1mr"]] + v[idx["D1mw"]]) / n, (v[idx["DLmr"]] + v[idx["DLmw"]]) / n
            exit
        }' "$out")
    [[ -n $parsed ]] || die "could not parse events/summary in $out" "$out"
    echo "$parsed"
}

# Hardware L1D load misses per iteration for the whole process (setup included), or n/a.
hardware() { # $1 = bench binary, $2 = benchmark name
    if [[ $PERF_OK != 1 ]]; then
        echo "n/a"
        return
    fi
    # shellcheck disable=SC2046
    perf stat -x, -e L1-dcache-load-misses "$1" $(bench_args "$2") 2>&1 >/dev/null |
        awk -F, -v n="$ITERATIONS" '/L1-dcache-load-misses/ { printf "%.3f\n", $1 / n }'
}

TABLE="$WORK/table.md"
{
    echo "| Commit | Change | L1D misses / add+cancel | L1D misses / match | LL misses / match | perf L1D / match (whole process) |"
    echo "|---|---|---|---|---|---|"
} >"$TABLE"

# Plain loop, not a pipeline: set -e must stay in force so a failed build or run aborts.
for c in "${COMMITS[@]}"; do
    hash=$(git -C "$ROOT" rev-parse --short "$c")
    subject=$(git -C "$ROOT" log -1 --format=%s "$c")
    echo "cachestat: $hash $subject" >&2
    bench=$(build "$c") || exit 1
    add=$(simulate "$bench" BM_AddCancel) || exit 1
    match=$(simulate "$bench" BM_AddMatch) || exit 1
    hw=$(hardware "$bench" BM_AddMatch)
    read -r addL1 _ <<<"$add"
    read -r matchL1 matchLL <<<"$match"
    echo "| \`$hash\` | $subject | $addL1 | $matchL1 | $matchLL | $hw |" >>"$TABLE"
done

{
    echo
    echo "callgrind --cache-sim=yes, ${CACHE[*]}, ${ITERATIONS} iterations per benchmark at depth 1000;"
    echo "misses counted only inside the benchmark function. perf column requires an exposed PMU."
} >>"$TABLE"

cat "$TABLE"
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
    cat "$TABLE" >>"$GITHUB_STEP_SUMMARY"
fi
