#!/usr/bin/env bash
set -euo pipefail

# Runs perf profiling for toycaml GC binaries with strict safety checks.
# Default focus is the parallel GC; lock/tas can be enabled via VARIANTS.
#
# Example:
#   ./perf_parallel_gc.sh
#   VARIANTS="parallel lock tas" DEPTHS="14" THREADS="1 2 4 8" RUNS=3 ./perf_parallel_gc.sh

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -g -fno-omit-frame-pointer -pthread}
VARIANTS=${VARIANTS:-"parallel"}
DEPTHS=${DEPTHS:-"14"}
THREADS=${THREADS:-"1 2 4 8"}
RUNS=${RUNS:-2}
PERF_FREQ=${PERF_FREQ:-999}
TOP_N=${TOP_N:-20}
PERCENT_LIMIT=${PERCENT_LIMIT:-0.5}

OUT_BASE=${OUT_BASE:-"$SCRIPT_DIR/perf_reports"}
STAMP=$(date +"%Y%m%d_%H%M%S")
OUT_DIR=${OUT_DIR:-"$OUT_BASE/run_$STAMP"}

LOCK_EXE="$OUT_DIR/toycaml_gc_lock_perf"
TAS_EXE="$OUT_DIR/toycaml_gc_tas_perf"
PARALLEL_EXE="$OUT_DIR/toycaml_gc_parallel_perf"

PERF_EVENTS="task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses"

usage() {
  cat <<'EOF'
Usage: ./perf_parallel_gc.sh

Environment overrides:
  VARIANTS      Space-separated set from: parallel lock tas   (default: "parallel")
  DEPTHS        Space-separated positive integers              (default: "14")
  THREADS       Space-separated positive integers              (default: "1 2 4 8")
  RUNS          Positive integer repeats per config            (default: 2)
  CFLAGS        Compiler flags                                 (default: -O2 -g -fno-omit-frame-pointer -pthread)
  PERF_FREQ     perf record sample frequency                   (default: 999)
  TOP_N         top hotspot symbols to extract                 (default: 20)
  PERCENT_LIMIT minimum overhead shown in reports              (default: 0.5)
  OUT_DIR       explicit output directory                      (default: benchmarking/perf_reports/run_<timestamp>)

Outputs:
  - perf_stats.csv              : compact numeric counters per run
  - stat_*.txt                  : raw perf stat outputs
  - perf_*.data                 : perf record data files
  - report_*.txt                : human-readable perf reports
  - hotspots_*.csv              : top symbols from perf report
  - annotate_top_*.txt          : perf annotate for top symbol (when available)
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

validate_positive_int() {
  local label=$1
  local value=$2
  if ! [[ "$value" =~ ^[0-9]+$ ]] || [[ "$value" -lt 1 ]]; then
    echo "$label must be a positive integer, got: $value"
    exit 1
  fi
}

validate_int_list() {
  local label=$1
  local list=$2
  local v
  for v in $list; do
    if ! [[ "$v" =~ ^[0-9]+$ ]] || [[ "$v" -lt 1 ]]; then
      echo "$label contains non-positive-integer value: $v"
      exit 1
    fi
  done
}

variant_exe_path() {
  local variant=$1
  case "$variant" in
    lock) echo "$LOCK_EXE" ;;
    tas) echo "$TAS_EXE" ;;
    parallel) echo "$PARALLEL_EXE" ;;
    *)
      echo "Unknown variant: $variant"
      exit 1
      ;;
  esac
}

sanitize_metric() {
  local raw=$1
  raw=$(echo "$raw" | tr -d '[:space:]')
  if [[ -z "$raw" ]]; then
    echo "nan"
    return
  fi
  if [[ "$raw" == "<notsupported>" || "$raw" == "<notcounted>" || "$raw" == "<notcounted>" ]]; then
    echo "nan"
    return
  fi
  echo "$raw"
}

metric_from_stat() {
  local stat_file=$1
  local key=$2
  awk -F, -v key="$key" '
    $3 == key {
      gsub(/ /, "", $1)
      print $1
      exit
    }
  ' "$stat_file"
}

build_variant() {
  local variant=$1
  local exe=$2
  case "$variant" in
    lock)
      "$CC" $CFLAGS "$PROJECT_ROOT/runtime.c" "$PROJECT_ROOT/semi_space_gc.c" \
        "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" -o "$exe"
      ;;
    tas)
      "$CC" $CFLAGS "$PROJECT_ROOT/runtime.c" "$PROJECT_ROOT/semi_space_gc_TAS_alloc.c" \
        "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" -o "$exe"
      ;;
    parallel)
      "$CC" $CFLAGS "$PROJECT_ROOT/runtime.c" "$PROJECT_ROOT/semi_space_gc_parallel_gc.c" \
        "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" -o "$exe"
      ;;
    *)
      echo "Unknown variant during build: $variant"
      exit 1
      ;;
  esac
}

echo "Preflight checks..."
require_cmd "$CC"
require_cmd perf
require_cmd awk
validate_positive_int "RUNS" "$RUNS"
validate_positive_int "PERF_FREQ" "$PERF_FREQ"
validate_positive_int "TOP_N" "$TOP_N"
validate_int_list "DEPTHS" "$DEPTHS"
validate_int_list "THREADS" "$THREADS"

if [[ ! -f "$PROJECT_ROOT/runtime.c" || ! -f "$PROJECT_ROOT/semi_space_gc.c" || ! -f "$PROJECT_ROOT/semi_space_gc_TAS_alloc.c" || ! -f "$PROJECT_ROOT/semi_space_gc_parallel_gc.c" || ! -f "$PROJECT_ROOT/tests/binary_tree_multithreaded.c" ]]; then
  echo "Could not find required source files under project root: $PROJECT_ROOT"
  exit 1
fi

for v in $VARIANTS; do
  case "$v" in
    lock|tas|parallel) ;;
    *)
      echo "VARIANTS contains unsupported entry: $v"
      echo "Supported: parallel lock tas"
      exit 1
      ;;
  esac
done

mkdir -p "$OUT_DIR"

echo "Checking perf permissions..."
if ! perf stat -x, -e cycles -- sleep 0.01 >/dev/null 2>&1; then
  cat <<'EOF'
perf cannot collect counters with current permissions.
Try one of:
  1) sudo sysctl -w kernel.perf_event_paranoid=1
  2) sudo sysctl -w kernel.kptr_restrict=0
  3) run this script with sudo (if acceptable in your environment)
EOF
  exit 1
fi

echo "Building binaries into: $OUT_DIR"
for v in $VARIANTS; do
  exe=$(variant_exe_path "$v")
  build_variant "$v" "$exe"
done

STATS_CSV="$OUT_DIR/perf_stats.csv"
echo "variant,depth,threads,run,task_clock_ms,cycles,instructions,ipc,branches,branch_misses,cache_references,cache_misses,elapsed_sec" > "$STATS_CSV"

echo "Running perf stat loops..."
for v in $VARIANTS; do
  exe=$(variant_exe_path "$v")
  for d in $DEPTHS; do
    for t in $THREADS; do
      for run in $(seq 1 "$RUNS"); do
        stat_file="$OUT_DIR/stat_${v}_d${d}_t${t}_r${run}.txt"
        echo "  [$v] depth=$d threads=$t run=$run"
        perf stat -x, -d -d -d -e "$PERF_EVENTS" -o "$stat_file" -- "$exe" "$d" "$t" >/dev/null 2>&1

        task_clock=$(sanitize_metric "$(metric_from_stat "$stat_file" "task-clock")")
        cycles=$(sanitize_metric "$(metric_from_stat "$stat_file" "cycles")")
        instructions=$(sanitize_metric "$(metric_from_stat "$stat_file" "instructions")")
        branches=$(sanitize_metric "$(metric_from_stat "$stat_file" "branches")")
        branch_misses=$(sanitize_metric "$(metric_from_stat "$stat_file" "branch-misses")")
        cache_references=$(sanitize_metric "$(metric_from_stat "$stat_file" "cache-references")")
        cache_misses=$(sanitize_metric "$(metric_from_stat "$stat_file" "cache-misses")")
        elapsed=$(sanitize_metric "$(metric_from_stat "$stat_file" "time elapsed")")

        ipc="nan"
        if [[ "$cycles" != "nan" && "$instructions" != "nan" ]]; then
          ipc=$(awk -v i="$instructions" -v c="$cycles" 'BEGIN { if (c > 0) printf "%.6f", i / c; else print "nan" }')
        fi

        echo "$v,$d,$t,$run,$task_clock,$cycles,$instructions,$ipc,$branches,$branch_misses,$cache_references,$cache_misses,$elapsed" >> "$STATS_CSV"
      done
    done
  done
done

# Focus call-graph profiling on the heaviest config (largest depth, largest threads) per variant.
profile_depth=$(echo "$DEPTHS" | awk '{print $NF}')
profile_threads=$(echo "$THREADS" | awk '{print $NF}')

echo "Running perf record/report at depth=$profile_depth threads=$profile_threads ..."
for v in $VARIANTS; do
  exe=$(variant_exe_path "$v")
  data_file="$OUT_DIR/perf_${v}_d${profile_depth}_t${profile_threads}.data"
  report_file="$OUT_DIR/report_${v}_d${profile_depth}_t${profile_threads}.txt"
  hotspots_file="$OUT_DIR/hotspots_${v}_d${profile_depth}_t${profile_threads}.csv"
  annotate_file="$OUT_DIR/annotate_top_${v}_d${profile_depth}_t${profile_threads}.txt"

  echo "  [record] $v"
  perf record -F "$PERF_FREQ" -g --call-graph fp -o "$data_file" -- "$exe" "$profile_depth" "$profile_threads" >/dev/null 2>&1

  echo "  [report] $v"
  perf report --stdio --percent-limit "$PERCENT_LIMIT" -n -i "$data_file" > "$report_file"

  echo "overhead_pct,symbol" > "$hotspots_file"
  perf report --stdio --percent-limit 0 -n -i "$data_file" -F overhead,symbol \
    | awk -v top_n="$TOP_N" '
      /^[[:space:]]*[0-9]+\.[0-9]+%/ {
        pct=$1
        gsub(/%/, "", pct)
        sym=$2
        if (sym == "" || sym == "[.]") next
        print pct "," sym
        count++
        if (count >= top_n) exit
      }
    ' >> "$hotspots_file"

  top_symbol=$(awk -F, 'NR==2 {print $2}' "$hotspots_file")
  if [[ -n "${top_symbol:-}" ]]; then
    perf annotate --stdio --symbol "$top_symbol" -i "$data_file" > "$annotate_file" 2>/dev/null || true
  fi
done

cat <<EOF

Perf run complete.
Output directory: $OUT_DIR

Primary files:
  $STATS_CSV
  $OUT_DIR/hotspots_*_d${profile_depth}_t${profile_threads}.csv
  $OUT_DIR/report_*_d${profile_depth}_t${profile_threads}.txt

Quick views:
  column -s, -t "$STATS_CSV" | less -S
  for f in "$OUT_DIR"/hotspots_*.csv; do echo "== \$f =="; column -s, -t "\$f"; echo; done
EOF
