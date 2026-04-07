#!/usr/bin/env bash
set -euo pipefail

# Benchmarks semi_space_gc.c vs semi_space_gc_TAS_alloc.c using tests/binary_tree_multithreaded.c
# Intended to run inside WSL/Linux from any working directory.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -pthread}
DEPTHS=${DEPTHS:-"14 15"}
THREADS=${THREADS:-"1 2 4 8"}
REPEATS=${REPEATS:-1}
OUT_CSV=${OUT_CSV:-"$SCRIPT_DIR/benchmark_results.csv"}

LOCK_EXE="$SCRIPT_DIR/toycaml_gc_lock"
TAS_EXE="$SCRIPT_DIR/toycaml_gc_tas"
PARALLEL_EXE="$SCRIPT_DIR/toycaml_gc_parallel"

if [[ ! -f "$PROJECT_ROOT/runtime.c" || ! -f "$PROJECT_ROOT/semi_space_gc.c" || ! -f "$PROJECT_ROOT/semi_space_gc_TAS_alloc.c" || ! -f "$PROJECT_ROOT/semi_space_gc_parallel_gc.c" || ! -f "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" ]]; then
  echo "Could not find required source files under project root: $PROJECT_ROOT"
  exit 1
fi

if ! [[ "$REPEATS" =~ ^[0-9]+$ ]] || [[ "$REPEATS" -lt 1 ]]; then
  echo "REPEATS must be a positive integer, got: $REPEATS"
  exit 1
fi

echo "Building binaries..."
$CC $CFLAGS "$PROJECT_ROOT/runtime.c" "$PROJECT_ROOT/semi_space_gc.c" "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" -o "$LOCK_EXE"
$CC $CFLAGS "$PROJECT_ROOT/runtime.c" "$PROJECT_ROOT/semi_space_gc_TAS_alloc.c" "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" -o "$TAS_EXE"
$CC $CFLAGS "$PROJECT_ROOT/runtime.c" "$PROJECT_ROOT/semi_space_gc_parallel_gc.c" "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" -o "$PARALLEL_EXE"

echo "variant,depth,threads,run,elapsed_sec" > "$OUT_CSV"

run_one() {
  local variant=$1
  local exe=$2
  local depth=$3
  local nthreads=$4
  local run_id=$5

  if ! [[ "$depth" =~ ^[0-9]+$ ]] || ! [[ "$nthreads" =~ ^[0-9]+$ ]]; then
    echo "Depth and thread values must be positive integers. Got depth=$depth threads=$nthreads"
    exit 1
  fi

  local elapsed
  if command -v /usr/bin/time >/dev/null 2>&1; then
    elapsed=$(/usr/bin/time -f "%e" "$exe" "$depth" "$nthreads" >/dev/null 2>&1)
  else
    local t0 t1
    t0=$(date +%s%N)
    "$exe" "$depth" "$nthreads" >/dev/null 2>&1
    t1=$(date +%s%N)
    elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f", (b-a)/1000000000.0 }')
  fi

  echo "$variant,$depth,$nthreads,$run_id,$elapsed" >> "$OUT_CSV"
  printf "%-8s depth=%-3s threads=%-2s run=%-2s time=%ss\n" "$variant" "$depth" "$nthreads" "$run_id" "$elapsed"
}

echo "Running benchmark..."
for depth in $DEPTHS; do
  for nthreads in $THREADS; do
    for run in $(seq 1 "$REPEATS"); do
      run_one "lock" "$LOCK_EXE" "$depth" "$nthreads" "$run"
      run_one "tas" "$TAS_EXE" "$depth" "$nthreads" "$run"
      run_one "parallel" "$PARALLEL_EXE" "$depth" "$nthreads" "$run"
    done
  done
done

echo
echo "Benchmark complete. CSV written to: $OUT_CSV"
echo "You can summarize quickly with:"
echo '  awk -F, '\''NR>1 {k=$1","$2","$3; s[k]+=$5; c[k]++} END {for (k in s) print k","s[k]/c[k]}'\'' "'"$OUT_CSV"'" | sort'
