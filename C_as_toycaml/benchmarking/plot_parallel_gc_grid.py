#!/usr/bin/env python3
import argparse
import csv
import glob
import math
import os
import statistics
from collections import defaultdict


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def load_rows(csv_path):
    rows = []
    skipped_rows = 0
    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        required = {"depth", "user_threads", "gc_threads", "run", "elapsed_sec"}
        if reader.fieldnames is None or not required.issubset(set(reader.fieldnames)):
            raise ValueError("CSV is missing required columns.")

        for idx, r in enumerate(reader, start=2):
            try:
                elapsed_raw = (r.get("elapsed_sec") or "").strip()
                if not elapsed_raw:
                    raise ValueError("elapsed_sec is empty")
                rows.append(
                    {
                        "depth": int(r["depth"]),
                        "user_threads": int(r["user_threads"]),
                        "gc_threads": int(r["gc_threads"]),
                        "run": int(r["run"]),
                        "elapsed_sec": float(elapsed_raw),
                    }
                )
            except (ValueError, TypeError, KeyError) as e:
                skipped_rows += 1
                print(f"Warning: skipping malformed CSV row {idx}: {e}")

    if skipped_rows:
        print(f"Warning: skipped {skipped_rows} malformed row(s) from {csv_path}")

    return rows


def mean_by_config(rows):
    grouped = defaultdict(list)
    for r in rows:
        grouped[(r["depth"], r["user_threads"], r["gc_threads"])].append(r["elapsed_sec"])

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


def evaluate_completeness(rows, depths, user_threads, gc_threads):
    seen = {(r["depth"], r["user_threads"], r["gc_threads"]) for r in rows}
    missing = []
    for d in depths:
        for u in user_threads:
            for g in gc_threads:
                if (d, u, g) not in seen:
                    missing.append((d, u, g))
    return missing


def write_summary_csv(summary_path, stats):
    with open(summary_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["depth", "user_threads", "gc_threads", "n", "mean_sec", "std_sec", "min_sec", "max_sec"])
        for (depth, user_threads, gc_threads), s in sorted(stats.items()):
            w.writerow(
                [
                    depth,
                    user_threads,
                    gc_threads,
                    s["n"],
                    f"{s['mean']:.6f}",
                    f"{s['std']:.6f}",
                    f"{s['min']:.6f}",
                    f"{s['max']:.6f}",
                ]
            )


def write_best_gc_csv(best_gc_path, stats, depths, user_threads, gc_threads):
    with open(best_gc_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "depth",
                "user_threads",
                "best_gc_threads",
                "best_mean_sec",
                "baseline_gc_threads",
                "baseline_mean_sec",
                "speedup_vs_baseline",
            ]
        )

        baseline_gc = min(gc_threads) if gc_threads else None
        for d in depths:
            for u in user_threads:
                candidates = []
                for g in gc_threads:
                    s = stats.get((d, u, g))
                    if s is not None:
                        candidates.append((g, s["mean"]))

                if not candidates:
                    continue

                best_g, best_mean = min(candidates, key=lambda x: x[1])
                baseline_mean = math.nan
                speedup = math.nan
                if baseline_gc is not None:
                    baseline_s = stats.get((d, u, baseline_gc))
                    if baseline_s is not None:
                        baseline_mean = baseline_s["mean"]
                        if best_mean > 0:
                            speedup = baseline_mean / best_mean

                w.writerow(
                    [
                        d,
                        u,
                        best_g,
                        f"{best_mean:.6f}",
                        baseline_gc if baseline_gc is not None else "nan",
                        "nan" if math.isnan(baseline_mean) else f"{baseline_mean:.6f}",
                        "nan" if math.isnan(speedup) else f"{speedup:.6f}",
                    ]
                )


def main():
    parser = argparse.ArgumentParser(description="Plot benchmark_parallel_gc_grid.csv")
    parser.add_argument(
        "--csv",
        default=os.path.join(SCRIPT_DIR, "benchmark_parallel_gc_grid.csv"),
        help="Input CSV path",
    )
    parser.add_argument(
        "--out-dir",
        default=os.path.join(SCRIPT_DIR, "benchmark_parallel_gc_plots"),
        help="Output directory for PNGs/CSVs",
    )
    parser.add_argument("--no-plot", action="store_true", help="Only evaluate and write summary CSVs")
    parser.add_argument(
        "--strict-grid",
        action="store_true",
        help="Fail if any depth/user_threads/gc_threads combination is missing",
    )
    args = parser.parse_args()

    rows = load_rows(args.csv)
    if not rows:
        raise ValueError("CSV has no valid data rows.")

    stats = mean_by_config(rows)
    depths = sorted({r["depth"] for r in rows})
    user_threads = sorted({r["user_threads"] for r in rows})
    gc_threads = sorted({r["gc_threads"] for r in rows})

    missing = evaluate_completeness(rows, depths, user_threads, gc_threads)
    if missing:
        preview = ", ".join([f"(d={d},u={u},g={g})" for d, u, g in missing[:8]])
        tail = " ..." if len(missing) > 8 else ""
        msg = f"CSV has missing depth/user/gc combinations: {preview}{tail}"
        if args.strict_grid:
            raise ValueError(msg)
        print(f"Warning: {msg}")

    os.makedirs(args.out_dir, exist_ok=True)

    # Remove stale images from prior runs.
    for old_png in glob.glob(os.path.join(args.out_dir, "time_vs_user_threads_depth_*.png")):
        os.remove(old_png)
    for old_png in glob.glob(os.path.join(args.out_dir, "time_vs_gc_threads_depth_*.png")):
        os.remove(old_png)
    stale_fixed_user = os.path.join(args.out_dir, "time_vs_depth_user_threads_4.png")
    if os.path.exists(stale_fixed_user):
        os.remove(stale_fixed_user)

    summary_path = os.path.join(args.out_dir, "summary_stats.csv")
    best_gc_path = os.path.join(args.out_dir, "best_gc_per_depth_user.csv")
    write_summary_csv(summary_path, stats)
    write_best_gc_csv(best_gc_path, stats, depths, user_threads, gc_threads)

    print("Logical evaluation (mean of config means):")
    overall_mean = sum(s["mean"] for s in stats.values()) / len(stats)
    print(f"  overall: {overall_mean:.6f} sec")
    print(f"  depths: {depths}")
    print(f"  user_threads: {user_threads}")
    print(f"  gc_threads: {gc_threads}")
    print(f"Summary CSV written: {summary_path}")
    print(f"Best-GC CSV written: {best_gc_path}")

    if args.no_plot:
        print("Skipping plot generation due to --no-plot")
        return

    try:
        import matplotlib.pyplot as plt
    except ImportError as e:
        raise RuntimeError(
            "matplotlib is required for plotting. Install python3-matplotlib or use a venv."
        ) from e

    # Plot 1: For each depth, mean time vs user_threads; one line per gc_threads.
    for depth in depths:
        plt.figure(figsize=(9, 5))
        for g in gc_threads:
            y_vals = []
            for u in user_threads:
                s = stats.get((depth, u, g))
                y_vals.append(float("nan") if s is None else s["mean"])
            plt.plot(user_threads, y_vals, marker="o", linewidth=2, label=f"gc={g}")

        plt.title(f"Mean Time vs User Threads (depth={depth})")
        plt.xlabel("User Threads")
        plt.ylabel("Elapsed Time (s)")
        plt.xticks(user_threads)
        plt.grid(True, alpha=0.3)
        plt.legend(title="GC Threads")
        plt.tight_layout()
        out_path = os.path.join(args.out_dir, f"time_vs_user_threads_depth_{depth}.png")
        plt.savefig(out_path, dpi=150)
        plt.close()

    # Plot 2: For each depth, mean time vs gc_threads; one line per user_threads.
    for depth in depths:
        plt.figure(figsize=(9, 5))
        for u in user_threads:
            y_vals = []
            for g in gc_threads:
                s = stats.get((depth, u, g))
                y_vals.append(float("nan") if s is None else s["mean"])
            plt.plot(gc_threads, y_vals, marker="o", linewidth=2, label=f"user={u}")

        plt.title(f"Mean Time vs GC Threads (depth={depth})")
        plt.xlabel("GC Threads")
        plt.ylabel("Elapsed Time (s)")
        plt.xticks(gc_threads)
        plt.grid(True, alpha=0.3)
        plt.legend(title="User Threads")
        plt.tight_layout()
        out_path = os.path.join(args.out_dir, f"time_vs_gc_threads_depth_{depth}.png")
        plt.savefig(out_path, dpi=150)
        plt.close()

    # Plot 3: Mean time vs depth for fixed user_threads=4; one line per gc_threads.
    fixed_user_threads = 4
    if fixed_user_threads in user_threads:
        plt.figure(figsize=(9, 5))
        for g in gc_threads:
            y_vals = []
            for depth in depths:
                s = stats.get((depth, fixed_user_threads, g))
                y_vals.append(float("nan") if s is None else s["mean"])
            plt.plot(depths, y_vals, marker="o", linewidth=2, label=f"gc={g}")

        plt.title("Mean Time vs Depth (user_threads=4)")
        plt.xlabel("Tree Depth")
        plt.ylabel("Elapsed Time (s)")
        plt.xticks(depths)
        plt.grid(True, alpha=0.3)
        plt.legend(title="GC Threads")
        plt.tight_layout()
        out_path = os.path.join(args.out_dir, "time_vs_depth_user_threads_4.png")
        plt.savefig(out_path, dpi=150)
        plt.close()
    else:
        print("Warning: user_threads=4 not present in data; skipping time_vs_depth_user_threads_4.png")

    print(f"Plots written to: {args.out_dir}")


if __name__ == "__main__":
    main()
