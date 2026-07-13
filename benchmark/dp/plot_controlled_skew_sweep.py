#!/usr/bin/env python3
import argparse
import csv
import math
import os
from collections import defaultdict
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib-codex")

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[2]
QUERY_ORDER = ["q01", "q05", "q06", "q14", "q19"]
WHALE_COLORS = {
    "50": "#0072b2",
    "150": "#d55e00",
    "450": "#009e73",
}
MECHANISM_COLORS = {
    "DP standard": "#d55e00",
    "DP elastic": "#0072b2",
    "SAA median": "#4dff4d",
    "SAA average": "#009900",
}
MECHANISM_MARKERS = {
    "DP standard": "o",
    "DP elastic": "^",
    "SAA median": "s",
    "SAA average": "D",
}


def norm_path(value):
    path = Path(value)
    if not path.is_absolute():
        path = ROOT / path
    return str(path.resolve())


def to_float(value):
    if value is None or value == "":
        return None
    return float(value)


def median(values):
    values = sorted(value for value in values if value is not None and math.isfinite(value))
    if not values:
        return None
    mid = len(values) // 2
    if len(values) % 2:
        return values[mid]
    return (values[mid - 1] + values[mid]) / 2.0


def read_metadata(path):
    variants = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            row["db_norm"] = norm_path(row["db"])
            row["remap_percent_num"] = to_float(row["remap_percent"]) or 0.0
            row["whales_per_sf_num"] = to_float(row["whales_per_sf"])
            variants[row["db_norm"]] = row
    return variants


def read_stability(path, variants):
    rows = []
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["success"] != "true" or row["median_cv"] == "":
                continue
            variant = variants.get(norm_path(row["db"]))
            if not variant:
                continue
            rows.append(
                {
                    **variant,
                    "query": row["query"],
                    "median_cv": float(row["median_cv"]),
                    "max_cv": float(row["max_cv"]) if row["max_cv"] else None,
                    "aggregate_rows": int(row["aggregate_rows"]),
                }
            )
    return rows


def mechanism_label(row):
    if row["mode"] == "dp_standard":
        return "DP standard"
    if row["mode"] == "dp_elastic":
        return "DP elastic"
    if row["mode"] == "dp_sass" and row["release"] == "median":
        return "SAA median"
    if row["mode"] == "dp_sass" and row["release"] == "average":
        return "SAA average"
    return None


def read_utility(path, variants):
    grouped = defaultdict(list)
    if not path.exists():
        return []
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or "mode" not in reader.fieldnames or "db_path" not in reader.fieldnames:
            return []
        for row in reader:
            label = mechanism_label(row)
            if not label or row["success"] != "true":
                continue
            if row.get("bound_multiplier") not in ("1", "1.0", "1.0000000000000000"):
                continue
            error = to_float(row.get("median_error_pct"))
            recall = to_float(row.get("recall"))
            time_ms = to_float(row.get("time_ms"))
            if error is None or recall is None or recall <= 0:
                continue
            variant = variants.get(norm_path(row["db_path"]))
            if not variant:
                continue
            key = (variant["variant"], row["query"], label)
            grouped[key].append({**variant, "query": row["query"], "mechanism": label, "error_pct": error, "time_ms": time_ms})

    rows = []
    for (_variant, _query, _label), values in grouped.items():
        first = values[0]
        rows.append(
            {
                **first,
                "median_error_pct": median([value["error_pct"] for value in values]),
                "median_time_ms": median([value["time_ms"] for value in values]),
                "runs": len(values),
            }
        )
    return rows


def write_merged(path, stability_rows, utility_rows):
    stability_by_key = {(row["variant"], row["query"]): row for row in stability_rows}
    fieldnames = [
        "variant",
        "query",
        "mechanism",
        "remap_percent",
        "whales_per_sf",
        "n_whales",
        "median_cv",
        "max_cv",
        "median_error_pct",
        "median_time_ms",
        "runs",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for utility in utility_rows:
            stability = stability_by_key.get((utility["variant"], utility["query"]))
            if not stability:
                continue
            writer.writerow(
                {
                    "variant": utility["variant"],
                    "query": utility["query"],
                    "mechanism": utility["mechanism"],
                    "remap_percent": utility["remap_percent"],
                    "whales_per_sf": utility["whales_per_sf"],
                    "n_whales": utility["n_whales"],
                    "median_cv": stability["median_cv"],
                    "max_cv": stability["max_cv"],
                    "median_error_pct": utility["median_error_pct"],
                    "median_time_ms": utility["median_time_ms"],
                    "runs": utility["runs"],
                }
            )


def setup_style():
    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 10,
            "axes.titlesize": 11,
            "axes.labelsize": 10,
            "legend.fontsize": 9,
            "xtick.labelsize": 9,
            "ytick.labelsize": 9,
            "axes.linewidth": 0.8,
        }
    )


def plot_stability(rows, output):
    setup_style()
    fig, axes = plt.subplots(1, len(QUERY_ORDER), figsize=(12.2, 3.35), sharey=True)
    for ax, query in zip(axes, QUERY_ORDER):
        q_rows = [row for row in rows if row["query"] == query]
        baseline = next((row for row in q_rows if row["variant"] == "baseline"), None)
        for whales in ["50", "150", "450"]:
            series = []
            if baseline:
                series.append((0.0, 100.0 * baseline["median_cv"]))
            series.extend(
                sorted(
                    [
                        (row["remap_percent_num"], 100.0 * row["median_cv"])
                        for row in q_rows
                        if row["whales_per_sf"] == whales
                    ]
                )
            )
            if len(series) < 2:
                continue
            ax.plot(
                [item[0] for item in series],
                [item[1] for item in series],
                marker="o",
                linewidth=2,
                markersize=4,
                color=WHALE_COLORS[whales],
                label=f"{whales} whales/SF",
            )
        ax.set_title(query.upper(), fontweight="bold")
        ax.set_yscale("log")
        ax.set_xticks([0, 10, 30, 60])
        ax.set_xticklabels(["0", "10", "30", "60"])
        ax.grid(True, which="major", color="#d8d8d8", linewidth=0.7)
        ax.grid(True, which="minor", axis="y", color="#eeeeee", linewidth=0.4)
    axes[0].set_ylabel("Median lane CV (%)")
    axes[0].legend(loc="upper left", frameon=False)
    fig.supxlabel("Orders remapped (%)", y=0.04)
    fig.suptitle("Controlled skew changes SAA lane stability", y=1.05, fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0.02, 0.06, 1.0, 0.98], w_pad=1.2)
    fig.savefig(output, dpi=240, bbox_inches="tight")


def plot_utility_vs_cv(stability_rows, utility_rows, output):
    if not utility_rows:
        return False
    setup_style()
    cv_by_key = {(row["variant"], row["query"]): 100.0 * row["median_cv"] for row in stability_rows}
    rows = []
    for row in utility_rows:
        cv = cv_by_key.get((row["variant"], row["query"]))
        if cv is None:
            continue
        rows.append({**row, "cv_pct": cv})
    if not rows:
        return False

    fig, axes = plt.subplots(1, len(QUERY_ORDER), figsize=(12.2, 3.55), sharey=True)
    for ax, query in zip(axes, QUERY_ORDER):
        q_rows = [row for row in rows if row["query"] == query]
        for mechanism in ["DP standard", "DP elastic", "SAA median", "SAA average"]:
            series = sorted([row for row in q_rows if row["mechanism"] == mechanism], key=lambda row: row["cv_pct"])
            if not series:
                continue
            ax.plot(
                [row["cv_pct"] for row in series],
                [max(row["median_error_pct"], 0.001) for row in series],
                marker=MECHANISM_MARKERS[mechanism],
                linewidth=1.8,
                markersize=4,
                color=MECHANISM_COLORS[mechanism],
                label=mechanism,
            )
        ax.set_title(query.upper(), fontweight="bold")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.grid(True, which="major", color="#d8d8d8", linewidth=0.7)
        ax.grid(True, which="minor", color="#eeeeee", linewidth=0.4)
    axes[0].set_ylabel("Median error (%)")
    axes[0].legend(loc="upper left", frameon=False)
    fig.supxlabel("Median lane CV (%)", y=0.04)
    fig.suptitle("Utility as SAA lane stability degrades", y=1.05, fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0.02, 0.06, 1.0, 0.98], w_pad=1.2)
    fig.savefig(output, dpi=240, bbox_inches="tight")
    return True


def main():
    parser = argparse.ArgumentParser(description="Plot controlled SAA skew-sweep stability and utility.")
    parser.add_argument("--metadata", default="benchmark/dp/skew_sweep/controlled_skew_sf30_metadata.csv")
    parser.add_argument("--stability", default="benchmark/dp/skew_sweep/controlled_skew_sf30_stability_summary.csv")
    parser.add_argument("--utility", default="benchmark/dp/skew_sweep/controlled_skew_sf30_allmodes_3run_results.csv")
    parser.add_argument("--out-dir", default="benchmark/dp/skew_sweep")
    args = parser.parse_args()

    out_dir = ROOT / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    variants = read_metadata(ROOT / args.metadata)
    stability_rows = read_stability(ROOT / args.stability, variants)
    utility_rows = read_utility(ROOT / args.utility, variants)

    stability_plot = out_dir / "controlled_skew_sf30_stability.png"
    utility_plot = out_dir / "controlled_skew_sf30_utility_vs_cv.png"
    merged_csv = out_dir / "controlled_skew_sf30_utility_stability.csv"
    plot_stability(stability_rows, stability_plot)
    print(stability_plot)
    if utility_rows:
        write_merged(merged_csv, stability_rows, utility_rows)
        print(merged_csv)
    if plot_utility_vs_cv(stability_rows, utility_rows, utility_plot):
        print(utility_plot)


if __name__ == "__main__":
    main()
