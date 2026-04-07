#!/usr/bin/env python3
import argparse
import csv
import os
import math
import statistics
import glob
from collections import defaultdict


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def load_rows(csv_path):
    rows = []
    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        required = {"variant", "depth", "threads", "run", "elapsed_sec"}
        if reader.fieldnames is None or not required.issubset(set(reader.fieldnames)):
            raise ValueError("CSV is missing required columns.")
        for r in reader:
            rows.append(
                {
                    "variant": r["variant"].strip(),
                    "depth": int(r["depth"]),
                    "threads": int(r["threads"]),
                    "run": int(r["run"]),
                    "elapsed_sec": float(r["elapsed_sec"]),
                }
            )
    return rows


def mean_by_variant_depth_threads(rows):
    grouped = defaultdict(list)
    for r in rows:
        grouped[(r["variant"], r["depth"], r["threads"])].append(r["elapsed_sec"])

    stats = {}
    for k, vals in grouped.items():
        mean_v = sum(vals) / len(vals)
        std_v = statistics.stdev(vals) if len(vals) > 1 else 0.0
        stats[k] = {
            "mean": mean_v,
            "std": std_v,
            "min": min(vals),
            "max": max(vals),
            "n": len(vals),
        }
    return stats


def evaluate_completeness(rows, variants, depths, threads):
    seen = {(r["variant"], r["depth"], r["threads"]) for r in rows}
    missing = []
    for v in variants:
        for d in depths:
            for t in threads:
                if (v, d, t) not in seen:
                    missing.append((v, d, t))
    return missing


def write_summary_csv(summary_path, stats):
    with open(summary_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["variant", "depth", "threads", "n", "mean_sec", "std_sec", "min_sec", "max_sec"])
        for (variant, depth, threads), s in sorted(stats.items()):
            w.writerow([
                variant,
                depth,
                threads,
                s["n"],
                f"{s['mean']:.6f}",
                f"{s['std']:.6f}",
                f"{s['min']:.6f}",
                f"{s['max']:.6f}",
            ])


def write_comparison_csv(comparison_path, stats, depths, threads):
    lock_key_exists = any(k[0] == "lock" for k in stats.keys())
    tas_key_exists = any(k[0] == "tas" for k in stats.keys())

    with open(comparison_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["depth", "threads", "lock_mean", "tas_mean", "parallel_mean", "speedup_lock_over_tas", "speedup_lock_over_parallel", "speedup_tas_over_parallel"])
        
        for d in depths:
            for t in threads:
                lock_s = stats.get(("lock", d, t))
                tas_s = stats.get(("tas", d, t))
                parallel_s = stats.get(("parallel", d, t))
                
                lock_mean = lock_s["mean"] if lock_s else None
                tas_mean = tas_s["mean"] if tas_s else None
                parallel_mean = parallel_s["mean"] if parallel_s else None
                
                speedup_tas = lock_mean / tas_mean if lock_mean is not None and tas_mean is not None and tas_mean > 0 else math.nan
                speedup_parallel = lock_mean / parallel_mean if lock_mean is not None and parallel_mean is not None and parallel_mean > 0 else math.nan
                speedup_tas_over_parallel = tas_mean / parallel_mean if tas_mean is not None and parallel_mean is not None and parallel_mean > 0 else math.nan

                w.writerow([
                    d,
                    t,
                    f"{lock_mean:.6f}" if lock_mean is not None else "nan",
                    f"{tas_mean:.6f}" if tas_mean is not None else "nan",
                    f"{parallel_mean:.6f}" if parallel_mean is not None else "nan",
                    "nan" if math.isnan(speedup_tas) else f"{speedup_tas:.6f}",
                    "nan" if math.isnan(speedup_parallel) else f"{speedup_parallel:.6f}",
                    "nan" if math.isnan(speedup_tas_over_parallel) else f"{speedup_tas_over_parallel:.6f}",
                ])

def main():
    parser = argparse.ArgumentParser(description="Plot benchmark_results.csv for lock vs tas allocators")
    parser.add_argument("--csv", default=os.path.join(SCRIPT_DIR, "benchmark_results.csv"), help="Input CSV path")
    parser.add_argument("--out-dir", default=os.path.join(SCRIPT_DIR, "benchmark_plots"), help="Output directory for PNGs")
    parser.add_argument("--no-plot", action="store_true", help="Only evaluate and write summary CSVs")
    args = parser.parse_args()

    rows = load_rows(args.csv)
    if not rows:
        raise ValueError("CSV has no data rows.")

    stats = mean_by_variant_depth_threads(rows)
    variants = sorted({r["variant"] for r in rows})
    depths = sorted({r["depth"] for r in rows})
    threads = sorted({r["threads"] for r in rows})

    missing = evaluate_completeness(rows, variants, depths, threads)
    if missing:
        preview = ", ".join([f"({v},d={d},t={t})" for v, d, t in missing[:8]])
        tail = " ..." if len(missing) > 8 else ""
        raise ValueError(f"CSV has missing variant/depth/thread combinations: {preview}{tail}")

    os.makedirs(args.out_dir, exist_ok=True)

    # Remove stale plot images from earlier runs so output reflects only current CSV.
    for old_png in glob.glob(os.path.join(args.out_dir, "time_vs_threads_depth_*.png")):
        os.remove(old_png)
    stale_speedup = os.path.join(args.out_dir, "tas_speedup.png")
    if os.path.exists(stale_speedup):
        os.remove(stale_speedup)
    stale_speedup2 = os.path.join(args.out_dir, "parallel_speedup.png")
    if os.path.exists(stale_speedup2):
        os.remove(stale_speedup2)

    summary_path = os.path.join(args.out_dir, "summary_stats.csv")
    comparison_path = os.path.join(args.out_dir, "lock_vs_tas_speedup.csv")
    write_summary_csv(summary_path, stats)
    write_comparison_csv(comparison_path, stats, depths, threads)

    overall = defaultdict(list)
    for (variant, _d, _t), s in stats.items():
        overall[variant].append(s["mean"])

    print("Logical evaluation (mean of config means):")
    for variant in sorted(overall.keys()):
        mean_of_means = sum(overall[variant]) / len(overall[variant])
        print(f"  {variant}: {mean_of_means:.6f} sec")

    print(f"Summary CSV written: {summary_path}")
    print(f"Comparison CSV written: {comparison_path}")

    if args.no_plot:
        print("Skipping plot generation due to --no-plot")
        return

    try:
        import matplotlib.pyplot as plt
    except ImportError as e:
        raise RuntimeError(
            "matplotlib is required for plotting. In WSL use either apt install python3-matplotlib or a venv."
        ) from e

    # Plot 1: one line-chart per depth (all variants against thread count)
    for depth in depths:
        plt.figure(figsize=(8, 5))
        for variant in variants:
            y_vals = []
            for t in threads:
                s = stats.get((variant, depth, t))
                y_vals.append(float("nan") if s is None else s["mean"])
            plt.plot(threads, y_vals, marker="o", linewidth=2, label=variant)

        plt.title(f"Mean Time vs Threads (depth={depth})")
        plt.xlabel("Threads")
        plt.ylabel("Elapsed Time (s)")
        plt.xticks(threads)
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        out_path = os.path.join(args.out_dir, f"time_vs_threads_depth_{depth}.png")
        plt.savefig(out_path, dpi=150)
        plt.close()

    # Plot 2: TAS speedup = lock_time / tas_time, one line per depth (if both exist)
    if "lock" in variants and "tas" in variants:
        plt.figure(figsize=(9, 5))
        for depth in depths:
            speedups = []
            for t in threads:
                lock_s = stats.get(("lock", depth, t))
                tas_s = stats.get(("tas", depth, t))
                if lock_s is None or tas_s is None or tas_s["mean"] == 0.0:
                    speedups.append(float("nan"))
                else:
                    speedups.append(lock_s["mean"] / tas_s["mean"])

            plt.plot(threads, speedups, marker="o", linewidth=2, label=f"depth={depth}")

        plt.axhline(1.0, color="black", linestyle="--", linewidth=1)
        plt.title("TAS Speedup over Lock (higher is better)")
        plt.xlabel("Threads")
        plt.ylabel("Speedup (lock / tas)")
        plt.xticks(threads)
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        speedup_path = os.path.join(args.out_dir, "tas_speedup.png")
        plt.savefig(speedup_path, dpi=150)
        plt.close()

    # Plot 3: Parallel speedup = lock_time / parallel_time, one line per depth
    if "lock" in variants and "parallel" in variants:
        plt.figure(figsize=(9, 5))
        for depth in depths:
            speedups = []
            for t in threads:
                lock_s = stats.get(("lock", depth, t))
                par_s = stats.get(("parallel", depth, t))
                if lock_s is None or par_s is None or par_s["mean"] == 0.0:
                    speedups.append(float("nan"))
                else:
                    speedups.append(lock_s["mean"] / par_s["mean"])

            plt.plot(threads, speedups, marker="o", linewidth=2, label=f"depth={depth}")

        plt.axhline(1.0, color="black", linestyle="--", linewidth=1)
        plt.title("Parallel Speedup over Lock (higher is better)")
        plt.xlabel("Threads")
        plt.ylabel("Speedup (lock / parallel)")
        plt.xticks(threads)
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        speedup_path = os.path.join(args.out_dir, "parallel_speedup.png")
        plt.savefig(speedup_path, dpi=150)
        plt.close()

    print(f"Plots written to: {args.out_dir}")


if __name__ == "__main__":
    main()
