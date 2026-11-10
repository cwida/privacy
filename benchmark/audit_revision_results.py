"""Collect runtime comparisons and execution anomalies without generating figures."""
import argparse
import csv
import json
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("collection", type=Path)
args = parser.parse_args()
fresh = args.collection / "results/duckdb2_revision_20261110"
previous = args.collection / "previous"
comparisons, anomalies = [], []

def read(path):
    if not path.exists():
        return []
    return list(csv.DictReader(path.open()))

def medians(path, value="median_ms", warmup=False):
    grouped = {}
    record_anomalies = path.is_relative_to(fresh)
    for row in read(path):
        if warmup and row.get("run") == "0":
            continue
        if row.get("status") not in (None, "ok", "executed_not_validated"):
            if record_anomalies:
                anomalies.append(dict(file=str(path.relative_to(args.collection)), query=row.get("query", ""),
                                      mode=row.get("mode", ""), status=row["status"], error=row.get("error", "")))
            continue
        if not row.get(value):
            continue
        number = float(row[value])
        if number < 0:
            if record_anomalies:
                anomalies.append(dict(file=str(path.relative_to(args.collection)), query=row.get("query", ""),
                                      mode=row.get("mode", ""), status="negative_runtime", error=""))
            continue
        mode = "".join(c for c in row.get("mode", "").lower() if c.isalnum())
        m = row.get("m") or ("0" if mode == "baseline" else "64")
        key = row.get("query", ""), mode, m
        grouped.setdefault(key, []).append(number)
    return {key: statistics.median(values) for key, values in grouped.items()}

def compare(name, old, new, value="median_ms", warmup=False):
    reference, measured = medians(old, value, warmup), medians(new, value, warmup)
    for key in sorted(reference.keys() & measured.keys()):
        before, after = reference[key], measured[key]
        comparisons.append(dict(benchmark=name, query=key[0], mode=key[1], m=key[2],
                                reference_ms=before, new_ms=after,
                                runtime_ratio=after / before if before else "",
                                inspect=bool(before and after / before > 1.2)))

compare("tpch_matched_main", fresh / "csv/as_tpch_sf30_main.csv", fresh / "csv/as_tpch_sf30.csv")
compare("tpch_previous_paper", previous / "as_tpch_m64.csv", fresh / "csv/as_tpch_sf30.csv")
compare("tpch_m_previous_paper", previous / "as_tpch_m_sweep.csv", fresh / "csv/as_tpch_sf30_m_sweep.csv")
compare("clickbench_previous_paper", previous / "as_clickbench_m64_m512.csv", fresh / "csv/as_clickbench_graviton.csv")
compare("tpcds_previous_paper", previous / "tpcds_sf30_plot_data.csv", fresh / "tpcds_sf30/summary.csv")
for kernel in ("approximate", "exact"):
    subpath = Path("ssb_sf30") / kernel / "threads_16_timings.csv"
    compare("ssb_" + kernel, previous / "graviton_5runs" / subpath, fresh / subpath, "query_ms")
    for workload in ("damico2006", "ahrq2016"):
        subpath = Path("meps") / kernel / workload / "threads_16_timings.csv"
        compare("meps_" + kernel + "_" + workload, previous / "graviton_5runs" / subpath,
                fresh / subpath, "query_ms", warmup=True)

for name, rows, fields in (
    ("runtime_comparison.csv", comparisons, ["benchmark", "query", "mode", "m", "reference_ms", "new_ms", "runtime_ratio", "inspect"]),
    ("execution_anomalies.csv", anomalies, ["file", "query", "mode", "status", "error"]),
):
    with (args.collection / name).open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
print(json.dumps(dict(comparisons=len(comparisons), inspect=sum(r["inspect"] for r in comparisons),
                      execution_anomalies=len(anomalies))))
