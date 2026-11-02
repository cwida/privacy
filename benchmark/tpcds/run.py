#!/usr/bin/env python3
"""Isolate TPC-DS queries in processes; all reported timings come from C++."""
import argparse
import csv
import json
import pathlib
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--runner", default="build/release/extension/privacy/as_tpcds_benchmark")
    parser.add_argument("--queries", help="Comma-separated query numbers; default: all 99")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    config = json.loads(pathlib.Path(args.config).read_text())
    query_ids = [int(q) for q in args.queries.split(",")] if args.queries else list(range(1, 100))
    if not query_ids or any(q < 1 or q > 99 for q in query_ids) or len(set(query_ids)) != len(query_ids):
        parser.error("Query numbers must be unique and between 1 and 99")
    output = pathlib.Path(config["out_dir"])
    manifest_path = output / "run_config.json"
    if manifest_path.exists() and json.loads(manifest_path.read_text()) != config:
        parser.error("Configuration changed: use a fresh out_dir to avoid mixing experiments")
    if not args.resume and any((output / f"q{q:02}.csv").exists() for q in query_ids):
        parser.error("Results already exist: use a fresh out_dir or --resume")
    subprocess.run([args.runner, args.config, "--prepare"], check=True)
    manifest_path.write_text(json.dumps(config, indent=2) + "\n")
    summary = []
    for query in query_ids:
        name = f"q{query:02}"
        path = output / f"{name}.csv"
        process_status = "completed"
        process_error = ""
        process_path = output / f"{name}_process.json"
        if args.resume and process_path.exists():
            state = json.loads(process_path.read_text())
            process_status, process_error = state["status"], state["error"]
        else:
            print(f"Running {name}", flush=True)
            with (output / f"{name}.log").open("w") as log:
                try:
                    result = subprocess.run(
                        [args.runner, args.config, str(query)], stdout=log, stderr=subprocess.STDOUT,
                        timeout=config.get("timeout_seconds", 120),
                    )
                    if result.returncode:
                        process_status = "process_error"
                        process_error = f"exit code {result.returncode}; see {name}.log"
                except subprocess.TimeoutExpired:
                    process_status = "timeout"
                    process_error = "Whole-query process timeout (both modes and all repetitions)"
            process_path.write_text(json.dumps(dict(status=process_status, error=process_error)))
        rows = list(csv.DictReader(path.open())) if path.exists() else []
        for mode in ("duckdb", "simd_asa_auto"):
            entries = [row for row in rows if row["mode"] == mode]
            measured = [row for row in entries if row["status"] == "ok"]
            complete = len(measured) == config.get("repetitions", 5)
            status = "executed_not_validated" if complete else (
                entries[-1]["status"] if entries and entries[-1]["status"] != "ok" else
                process_status if process_status != "completed" else "incomplete"
            )
            summary.append(dict(
                query=name, mode=mode, status=status,
                rewritten=entries[-1]["rewritten"] if entries else "",
                measured_runs=len(measured),
                median_ms=statistics.median(float(row["runtime_ms"]) for row in measured) if complete else "",
                empty_result=all(row["rows"] == "0" for row in measured) if complete else "",
                correctness="not_validated",
                error=entries[-1]["error"] if entries and entries[-1]["error"] else process_error,
            ))
        with (output / "summary.csv").open("w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=list(summary[0]))
            writer.writeheader()
            writer.writerows(summary)
        print(f"{name}: " + ", ".join(f"{r['mode']}={r['status']}" for r in summary[-2:]), flush=True)
    print(f"Results: {output / 'summary.csv'}")


if __name__ == "__main__":
    main()
