#!/usr/bin/env bash
set -euo pipefail

# Benchmarks only parallel GC across:
#   - tree depth
#   - mutator/user threads
#   - GC worker threads (compile-time NUM_GC_THREADS)
#
# CSV columns:
# depth,user_threads,gc_threads,run,elapsed_sec,binary

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -g -fno-omit-frame-pointer -pthread}
DEPTHS=${DEPTHS:-"10 12 14"}
USER_THREADS=${USER_THREADS:-"1 2 4 8"}
GC_THREADS=${GC_THREADS:-"1 2 4 8"}
REPEATS=${REPEATS:-3}
OUT_CSV=${OUT_CSV:-"$SCRIPT_DIR/benchmark_parallel_gc_grid.csv"}

BIN_DIR="$SCRIPT_DIR/.parallel_gc_bins"

usage() {
  cat <<'EOF'
Usage: ./benchmark_parallel_gc_grid.sh

Environment overrides:
  DEPTHS        Space-separated positive integers (default: "10 12 14")
  USER_THREADS  Space-separated positive integers (default: "1 2 4 8")
  GC_THREADS    Space-separated positive integers (default: "1 2 4 8")
  REPEATS       Positive integer (default: 3)
  CFLAGS        Compiler flags
  OUT_CSV       Output CSV path

Example:
  DEPTHS="12 14" USER_THREADS="2 4 8" GC_THREADS="1 2 4 8 16" REPEATS=2 ./benchmark_parallel_gc_grid.sh
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

require_cmd() {
  local cmd=$1
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Missing required command: $cmd"
    exit 1
  fi
}

validate_pos_int() {
  local label=$1
  local value=$2
  if ! [[ "$value" =~ ^[0-9]+$ ]] || [[ "$value" -lt 1 ]]; then
    echo "$label must be a positive integer, got: $value"
    exit 1
  fi
}

validate_pos_int_list() {
  local label=$1
  local values=$2
  local x
  for x in $values; do
    if ! [[ "$x" =~ ^[0-9]+$ ]] || [[ "$x" -lt 1 ]]; then
      echo "$label contains non-positive-integer value: $x"
      exit 1
    fi
  done
}

run_one() {
  local exe=$1
  local depth=$2
  local users=$3
  local gc_threads=$4
  local run_id=$5

  local elapsed
  if command -v /usr/bin/time >/dev/null 2>&1; then
    local tmp_time
    tmp_time=$(mktemp)
    /usr/bin/time -f "%e" -o "$tmp_time" "$exe" "$depth" "$users" >/dev/null 2>&1
    elapsed=$(tr -d '[:space:]' < "$tmp_time")
    rm -f "$tmp_time"
  else
    local t0 t1
    t0=$(date +%s%N)
    "$exe" "$depth" "$users" >/dev/null 2>&1
    t1=$(date +%s%N)
    elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.6f", (b-a)/1000000000.0 }')
  fi

  if [[ -z "$elapsed" ]]; then
    echo "Failed to collect elapsed time: depth=$depth users=$users gc_threads=$gc_threads run=$run_id"
    exit 1
  fi

  echo "$depth,$users,$gc_threads,$run_id,$elapsed,$exe" >> "$OUT_CSV"
  printf "depth=%-3s users=%-3s gc_threads=%-3s run=%-2s time=%ss\n" "$depth" "$users" "$gc_threads" "$run_id" "$elapsed"
}

echo "Preflight checks..."
require_cmd "$CC"
require_cmd awk
validate_pos_int "REPEATS" "$REPEATS"
validate_pos_int_list "DEPTHS" "$DEPTHS"
validate_pos_int_list "USER_THREADS" "$USER_THREADS"
validate_pos_int_list "GC_THREADS" "$GC_THREADS"

if [[ ! -f "$PROJECT_ROOT/runtime.c" || ! -f "$PROJECT_ROOT/semi_space_gc_parallel_gc.c" || ! -f "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" ]]; then
  echo "Could not find required source files under project root: $PROJECT_ROOT"
  exit 1
fi

mkdir -p "$BIN_DIR"
echo "depth,user_threads,gc_threads,run,elapsed_sec,binary" > "$OUT_CSV"

echo "Building one binary per GC thread count..."
for gc in $GC_THREADS; do
  exe="$BIN_DIR/toycaml_gc_parallel_gc${gc}"
  "$CC" $CFLAGS -DNUM_GC_THREADS="$gc" \
    "$PROJECT_ROOT/runtime.c" \
    "$PROJECT_ROOT/semi_space_gc_parallel_gc.c" \
    "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" \
    -o "$exe"
done

echo "Running benchmark grid..."
for depth in $DEPTHS; do
  for users in $USER_THREADS; do
    for gc in $GC_THREADS; do
      exe="$BIN_DIR/toycaml_gc_parallel_gc${gc}"
      for run in $(seq 1 "$REPEATS"); do
        run_one "$exe" "$depth" "$users" "$gc" "$run"
      done
    done
  done
done

echo
echo "Benchmark complete. CSV written to: $OUT_CSV"
echo "Quick mean summary by depth,user_threads,gc_threads:"
echo '  awk -F, '\''NR>1 {k=$1","$2","$3; s[k]+=$5; c[k]++} END {for (k in s) printf "%s,%.6f\n", k, s[k]/c[k]}'\'' "'"$OUT_CSV"'" | sort'
