#!/usr/bin/env bash
# Cache-miss measurement per optimization commit (roadmap 3.4).
#
# Every number in the README's performance table is wall-clock; the claim behind
# them is "fewer cache misses". This script measures the misses themselves. For
# each commit it checks out a worktree, builds the Release benchmark, and runs
# BM_AddCancel/1000 and BM_AddMatch/1000 under valgrind's cache simulator
# (callgrind --cache-sim=yes), reporting instructions and data-cache misses per
# operation.
#
# Differential, deliberately: each benchmark runs twice, for N and 2N iterations,
# and the per-operation cost is (second − first) / N. The simulation is
# deterministic, so everything that does not scale with the iteration count —
# the arena's construction (tens of MB zeroed), the depth-1000 fill, the
# harness — cancels exactly. (A first attempt scoped collection to the benchmark
# function instead; the constructor lives inside it and swamped the loop.)
#
# Simulation, deliberately too: it gives the same count to the digit every run
# and works anywhere Linux does, including a CI VM whose hypervisor hides the
# PMU. What it cannot say is how long a miss took — no prefetcher, no
# out-of-order overlap, no coherence traffic — so it proves *fewer*, while the
# ns table proves *faster*.
#
# Two depths, deliberately: at 1,000 resting orders the book fits a 32 KB L1D and
# every version since 1.1 misses zero times per operation — those wins were
# instruction count, and the table says so. At 10,000 the price arrays and slots
# outgrow L1, which is where a locality claim can actually be tested.
#
# Usage:  tools/cachestat.sh [--iterations N] [--depths "1000 10000"] [commit...]
#         default commits: the optimization commits from the README table + HEAD
# Output: a Markdown table on stdout (appended to $GITHUB_STEP_SUMMARY when set).
#         Any build, valgrind or parse failure aborts with that tool's log: an
#         empty cell is never silently produced.
# Env:    CACHESTAT_SKIP_VALGRIND=1 runs the bench natively for a pipeline smoke test
#         (counts print as n/a) — the only way to exercise this on macOS.
set -euo pipefail

ITERATIONS=50000
DEPTHS="1000 10000"
COMMITS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --iterations)
        ITERATIONS="$2"
        shift 2
        ;;
    --depths)
        DEPTHS="$2"
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
        tail -n 60 "$2" >&2
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

bench_args() { # $1 = benchmark name, $2 = depth, $3 = iterations
    echo "--benchmark_filter=^$1/$2\$ --benchmark_min_time=$3x --benchmark_min_warmup_time=0"
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

# Whole-run totals under the simulator: "Ir D1misses LLmisses" (reads + writes).
totals() { # $1 = bench binary, $2 = benchmark name, $3 = depth, $4 = iterations
    local log="$WORK/run.$2.$3.$4.log" out="$WORK/callgrind.$2.$3.$4.out"
    # --trace-children so a client that exec()s is still traced (without it the
    # image runs natively and no file is ever written).
    # shellcheck disable=SC2046
    valgrind --trace-children=yes --tool=callgrind --cache-sim=yes "${CACHE[@]}" \
        --callgrind-out-file="$out" "$1" $(bench_args "$2" "$3" "$4") >"$log" 2>&1 ||
        die "valgrind on $2/$3 ($4 iterations) failed" "$log"
    if [[ ! -s $out ]]; then
        ls -la "$WORK" >&2
        die "valgrind wrote no callgrind file for $2/$3 ($4 iterations)" "$log"
    fi
    # The callgrind file names its event columns once ("events:") and totals them
    # once ("summary:" in the header or "totals:" at the end).
    local parsed
    parsed=$(awk '
        /^events:/ { for (i = 2; i <= NF; i++) idx[$i] = i - 1 }
        /^(summary|totals):/ {
            for (i = 2; i <= NF; i++) v[i - 1] = $i
            print v[idx["Ir"]], v[idx["D1mr"]] + v[idx["D1mw"]], v[idx["DLmr"]] + v[idx["DLmw"]]
            exit
        }' "$out")
    [[ -n $parsed ]] || die "could not parse events/summary in $out" "$out"
    echo "$parsed"
}

# Per-operation cost: "Ir D1 LL" as the difference between a 2N-iteration run and
# an N-iteration run, divided by N. Everything constant cancels exactly; a
# difference of a handful of events (harness bookkeeping) rounds to zero rather
# than printing -0.000.
simulate() { # $1 = bench binary, $2 = benchmark name, $3 = depth
    if [[ $SKIP_VALGRIND == 1 ]]; then
        local log="$WORK/run.$2.$3.log"
        # shellcheck disable=SC2046
        "$1" $(bench_args "$2" "$3" "$ITERATIONS") >"$log" 2>&1 || die "bench $2/$3 failed" "$log"
        echo "n/a n/a n/a"
        return
    fi
    local first second
    first=$(totals "$1" "$2" "$3" "$ITERATIONS") || return 1
    second=$(totals "$1" "$2" "$3" $((ITERATIONS * 2))) || return 1
    awk -v n="$ITERATIONS" -v a="$first" -v b="$second" '
        function perop(i) { v = (y[i] - x[i]) / n; return (v < 0 && v > -0.01) ? 0 : v }
        BEGIN {
            split(a, x, " "); split(b, y, " ")
            printf "%.1f %.3f %.4f\n", perop(1), perop(2), perop(3)
        }'
}

TABLE="$WORK/table.md"
{
    echo "| Commit | Change | Depth | instr / add+cancel | L1D misses / add+cancel | instr / match | L1D misses / match | LL misses / match |"
    echo "|---|---|---|---|---|---|---|---|"
} >"$TABLE"

# Plain loop, not a pipeline: set -e must stay in force so a failed build or run aborts.
for c in "${COMMITS[@]}"; do
    hash=$(git -C "$ROOT" rev-parse --short "$c")
    subject=$(git -C "$ROOT" log -1 --format=%s "$c")
    echo "cachestat: $hash $subject" >&2
    bench=$(build "$c") || exit 1
    for depth in $DEPTHS; do
        add=$(simulate "$bench" BM_AddCancel "$depth") || exit 1
        match=$(simulate "$bench" BM_AddMatch "$depth") || exit 1
        read -r addIr addL1 _ <<<"$add"
        read -r matchIr matchL1 matchLL <<<"$match"
        echo "| \`$hash\` | $subject | $depth | $addIr | $addL1 | $matchIr | $matchL1 | $matchLL |" >>"$TABLE"
    done
done

{
    echo
    echo "callgrind --cache-sim=yes, ${CACHE[*]}; per-operation = (run of $((ITERATIONS * 2)) iterations − run of ${ITERATIONS}) / ${ITERATIONS},"
    echo "so construction, fill and harness cancel exactly. Simulated counts: deterministic, no timing. Hardware counters need a PMU the CI VM does not expose."
} >>"$TABLE"

cat "$TABLE"
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
    cat "$TABLE" >>"$GITHUB_STEP_SUMMARY"
fi
