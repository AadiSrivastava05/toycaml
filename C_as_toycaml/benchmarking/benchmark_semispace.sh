#!/usr/bin/env bash
set -euo pipefail

# Benchmarks available semi-space GC implementations using tests/binary_tree_multithreaded.c.
# Skips any GC source that is missing, unreadable, or fails to compile.
# Intended to run inside WSL/Linux from any working directory.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -pthread}
DEPTHS=${DEPTHS:-"15"}
THREADS=${THREADS:-"4 8"}
REPEATS=${REPEATS:-1}
OUT_CSV=${OUT_CSV:-"$SCRIPT_DIR/benchmark_results.csv"}

LOCK_EXE="$SCRIPT_DIR/toycaml_gc_lock"
TAS_EXE="$SCRIPT_DIR/toycaml_gc_tas"
PARALLEL_EXE="$SCRIPT_DIR/toycaml_gc_parallel"
WORK_STEAL_Q_EXE="$SCRIPT_DIR/toycaml_gc_work_steal_q"
BATCHED_EXE="$SCRIPT_DIR/toycaml_gc_batched"
GCLAB_EXE="$SCRIPT_DIR/toycaml_gc_gclab"

RUNTIME_C="$PROJECT_ROOT/runtime.c"
TEST_C="$PROJECT_ROOT/tests/binary_tree_multithreaded.c"

if [[ ! -f "$RUNTIME_C" || ! -r "$RUNTIME_C" || ! -f "$TEST_C" || ! -r "$TEST_C" ]]; then
  echo "Could not find or read required shared sources under project root: $PROJECT_ROOT"
  echo "Need: runtime.c and tests/binary_tree_multithreaded.c"
  exit 1
fi

if ! [[ "$REPEATS" =~ ^[0-9]+$ ]] || [[ "$REPEATS" -lt 1 ]]; then
  echo "REPEATS must be a positive integer, got: $REPEATS"
  exit 1
fi

# Each entry: variant_name|exe_path (exe must not contain '|')
BUILT_VARIANTS=()

try_build_variant() {
  local variant=$1
  local exe=$2
  local gc_src=$3

  if [[ ! -f "$gc_src" || ! -r "$gc_src" ]]; then
    echo "Skipping variant \"$variant\": source not found or not readable: $gc_src" >&2
    return 0
  fi
  echo "Building $variant ($gc_src) -> $exe"
  if ! $CC $CFLAGS "$RUNTIME_C" "$gc_src" "$TEST_C" -o "$exe" 2>&1; then
    echo "Skipping variant \"$variant\": compile failed for $gc_src" >&2
    rm -f "$exe"
    return 0
  fi
  BUILT_VARIANTS+=("${variant}|${exe}")
}

echo "Building binaries (missing or broken GC sources are skipped)..."
try_build_variant "lock" "$LOCK_EXE" "$PROJECT_ROOT/semi_space_gc.c"
try_build_variant "tas" "$TAS_EXE" "$PROJECT_ROOT/semi_space_gc_TAS_alloc.c"
try_build_variant "parallel" "$PARALLEL_EXE" "$PROJECT_ROOT/semi_space_gc_parallel_gc.c"
try_build_variant "work_steal_q" "$WORK_STEAL_Q_EXE" "$PROJECT_ROOT/semi_space_gc_work_steal_q.c"
try_build_variant "batched" "$BATCHED_EXE" "$PROJECT_ROOT/semis_space_gc_batched_gc.c"
try_build_variant "gclab" "$GCLAB_EXE" "$PROJECT_ROOT/semi_space_gc_gclab.c"

if [[ ${#BUILT_VARIANTS[@]} -eq 0 ]]; then
  echo "No variants could be built; nothing to benchmark." >&2
  exit 1
fi

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
    local tmp_time
    tmp_time=$(mktemp)
    /usr/bin/time -f "%e" -o "$tmp_time" "$exe" "$depth" "$nthreads" >/dev/null 2>&1
    elapsed=$(tr -d '[:space:]' < "$tmp_time")
    rm -f "$tmp_time"
  else
    local t0 t1
    t0=$(date +%s%N)
    "$exe" "$depth" "$nthreads" >/dev/null 2>&1
    t1=$(date +%s%N)
    elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f", (b-a)/1000000000.0 }')
  fi

  if [[ -z "$elapsed" ]]; then
    echo "Failed to collect elapsed time for $variant depth=$depth threads=$nthreads run=$run_id"
    exit 1
  fi

  echo "$variant,$depth,$nthreads,$run_id,$elapsed" >> "$OUT_CSV"
  printf "%-12s depth=%-3s threads=%-2s run=%-2s time=%ss\n" "$variant" "$depth" "$nthreads" "$run_id" "$elapsed"
}

BUILT_NAMES=()
for entry in "${BUILT_VARIANTS[@]}"; do
  BUILT_NAMES+=("${entry%%|*}")
done
echo "Running benchmark for variants: ${BUILT_NAMES[*]}"

for depth in $DEPTHS; do
  for nthreads in $THREADS; do
    for run in $(seq 1 "$REPEATS"); do
      for entry in "${BUILT_VARIANTS[@]}"; do
        variant=${entry%%|*}
        exe=${entry#*|}
        run_one "$variant" "$exe" "$depth" "$nthreads" "$run"
      done
    done
  done
done

echo
echo "Benchmark complete. CSV written to: $OUT_CSV"
echo "You can summarize quickly with:"
echo '  awk -F, '\''NR>1 {k=$1","$2","$3; s[k]+=$5; c[k]++} END {for (k in s) print k","s[k]/c[k]}'\'' "'"$OUT_CSV"'" | sort'
