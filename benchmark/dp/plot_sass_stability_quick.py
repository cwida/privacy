#!/usr/bin/env python3
import argparse
import csv
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-codex")

import matplotlib.pyplot as plt


QUERY_ORDER = ["q01", "q05", "q06", "q14", "q19"]
DATASET_LABELS = {
    "tpch": "TPC-H",
    "jcch": "JCC-H",
}
DATASET_COLORS = {
    "tpch": "#0072b2",
    "jcch": "#d55e00",
}


def read_rows(path):
    with path.open(newline="") as handle:
        rows = []
        for row in csv.DictReader(handle):
            if row["success"] != "true" or row["median_cv"] == "":
                continue
            rows.append(
                {
                    "dataset": row["dataset"],
                    "query": row["query"],
                    "bound_multiplier": float(row["bound_multiplier"]),
                    "median_cv_pct": 100.0 * float(row["median_cv"]),
                    "max_cv_pct": 100.0 * float(row["max_cv"]),
                }
            )
        return rows


def plot_stability(rows, output):
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 10,
            "axes.titlesize": 11,
            "axes.labelsize": 10,
            "legend.fontsize": 10,
            "xtick.labelsize": 9,
            "ytick.labelsize": 9,
            "axes.linewidth": 0.8,
        }
    )

    fig, axes = plt.subplots(1, len(QUERY_ORDER), figsize=(12.2, 3.35), sharey=True)
    x_ticks = [0.01, 0.1, 1.0, 10.0, 100.0]
    y_ticks = [0.3, 0.5, 1, 2, 5, 10, 20, 40]

    for ax, query in zip(axes, QUERY_ORDER):
        query_rows = [row for row in rows if row["query"] == query]
        for dataset in ["tpch", "jcch"]:
            series = sorted(
                [row for row in query_rows if row["dataset"] == dataset],
                key=lambda row: row["bound_multiplier"],
            )
            if not series:
                continue
            ax.plot(
                [row["bound_multiplier"] for row in series],
                [row["median_cv_pct"] for row in series],
                marker="o",
                markersize=4.2,
                linewidth=2.0,
                color=DATASET_COLORS[dataset],
                label=DATASET_LABELS[dataset],
            )
        ax.set_title(query.upper(), fontweight="bold")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks(x_ticks)
        ax.set_xticklabels(["0.01", "0.1", "1", "10", "100"])
        ax.set_yticks(y_ticks)
        ax.set_yticklabels([str(tick) for tick in y_ticks])
        ax.grid(True, which="major", color="#d8d8d8", linewidth=0.7)
        ax.grid(True, which="minor", axis="y", color="#eeeeee", linewidth=0.4)
        ax.tick_params(axis="x", rotation=35)
        ax.set_ylim(0.25, 45)

    axes[0].set_ylabel("Median lane CV (%)")
    fig.supxlabel("Bound multiplier", y=0.04)
    axes[0].legend(loc="upper left", frameon=False, handlelength=2.0, borderpad=0.2, labelspacing=0.3)
    fig.suptitle("SAA stability across 64 lanes on SF30", y=1.10, fontsize=13, fontweight="bold")
    fig.text(
	    0.5,
	    0.985,
	    "CV = stddev(lane outputs) / abs(mean lane output); lower is more stable",
	    ha="center",
	    va="center",
	    fontsize=9.5,
	)
    fig.tight_layout(rect=[0.02, 0.05, 1.0, 0.94], w_pad=1.2)
    fig.savefig(output, dpi=240, bbox_inches="tight")


def main():
    parser = argparse.ArgumentParser(description="Quick SAA lane-stability plot for TPCH/JCC-H.")
    parser.add_argument(
        "--input",
        default="benchmark/dp/tpch_jcch_sf30_all5_sass_stability_summary.csv",
        help="summary CSV from run_tpch_jcch_sass_stability.py",
    )
    parser.add_argument(
        "--output",
        default="benchmark/dp/tpch_jcch_sf30_sass_stability_quick.png",
        help="output PNG path",
    )
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    rows = read_rows(input_path)
    if not rows:
        raise RuntimeError(f"no successful stability rows in {input_path}")
    plot_stability(rows, output_path)
    print(output_path)


if __name__ == "__main__":
    main()
