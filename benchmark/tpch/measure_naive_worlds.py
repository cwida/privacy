"""Check the naive-baseline estimate (world 0 latency x 64) by timing all 64 worlds.

For every naive TPC-H query: prepare, warm up, then time EXECUTE for each of the 64 sampled worlds, REPS times.
Reports the sum over the 64 worlds (median per world) against 64 x the world-0 median, which is what
as_tpch_benchmark reports. The database must already contain the sample tables that the benchmark creates
(random_samples, random_samples_orders, lineitem_enhanced).

usage: measure_naive_worlds.py DUCKDB_BINARY DATABASE benchmark/tpch/tpch_naive_as_queries THREADS REPS OUT_CSV
"""
import csv
import re
import statistics
import subprocess
import sys
from pathlib import Path

binary, database, query_dir, threads, reps, out_csv = sys.argv[1:7]
reps = int(reps)
rows = []
for path in sorted(Path(query_dir).glob("q*.sql")):
    sql = path.read_text()
    prepare = sql[:sql.rfind("EXECUTE")]
    script = [f"SET threads={threads};", ".mode trash", prepare, "EXECUTE run_query(sample := 0);", ".timer on"]
    for rep in range(reps):
        for world in range(64):
            script.append(f"EXECUTE run_query(sample := {world});")
    out = subprocess.run([binary, "-readonly", database], input="\n".join(script), capture_output=True, text=True,
                         env={"DUCKDB_AGENT_MODE": "0", "PATH": "/usr/bin:/bin"})
    times = [float(x) * 1000 for x in re.findall(r"Run Time \(s\): real ([0-9.]+)", out.stdout + out.stderr)]
    if len(times) != 64 * reps or "Error" in out.stdout + out.stderr or min(times) <= 0:
        print(path.stem, "FAILED", len(times), (out.stderr or out.stdout)[-300:], flush=True)
        continue
    per_world = [statistics.median(times[rep * 64 + world] for rep in range(reps)) for world in range(64)]
    measured = sum(per_world)
    estimate = 64 * per_world[0]
    rows.append(dict(query=path.stem, world0_ms=round(per_world[0], 2), min_world_ms=round(min(per_world), 2),
                     max_world_ms=round(max(per_world), 2), sum_64_worlds_ms=round(measured, 1),
                     world0_x64_ms=round(estimate, 1), estimate_vs_measured_pct=round(100 * (estimate / measured - 1), 1)))
    print(rows[-1], flush=True)
with open(out_csv, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
diffs = [r["estimate_vs_measured_pct"] for r in rows]
print("queries:", len(rows), "| difference of world0 x 64 from the measured sum: median %+.1f%%, min %+.1f%%, max %+.1f%%, "
      "mean absolute %.1f%%" % (statistics.median(diffs), min(diffs), max(diffs), statistics.mean(abs(d) for d in diffs)))
total_est, total_meas = sum(r["world0_x64_ms"] for r in rows), sum(r["sum_64_worlds_ms"] for r in rows)
print("all queries together: estimate %.0f ms, measured %.0f ms, difference %+.1f%%" % (total_est, total_meas, 100 * (total_est / total_meas - 1)))
