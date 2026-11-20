#!/usr/bin/env python3
"""Isolate TPC-DS queries in processes; all reported timings come from C++."""
import argparse
import csv
import hashlib
import json
import pathlib
import statistics
import subprocess


def digest_file(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--runner", default="build/release/extension/privacy/as_tpcds_benchmark")
    parser.add_argument("--queries", help="Comma-separated query numbers; default: all 99")
    parser.add_argument("--resume", action="store_true")
    validation = parser.add_mutually_exclusive_group()
    validation.add_argument("--validate", action="store_true", help="Compare handwritten PAC SQL to automatic compilation")
    validation.add_argument("--validate-compiled", action="store_true", help="Compare PAC output to world-zero SQL (semantics diagnostic, not compiler correctness)")
    validation.add_argument("--validate-worlds", action="store_true", help="Run the separate 64-world AS reference checks")
    args = parser.parse_args()
    if args.validate_compiled or args.validate_worlds:
        args.validate = True
    config = json.loads(pathlib.Path(args.config).read_text())
    handwritten_dir = pathlib.Path(config.get("reference_queries_dir", "benchmark/tpcds/reference_queries") if args.validate_worlds else config.get("as_queries_dir", "benchmark/tpcds/as_queries"))
    default_ids = sorted(int(p.stem[1:]) for p in handwritten_dir.glob("q[0-9][0-9].sql")) if args.validate else list(range(1, 100))
    query_ids = [int(q) for q in args.queries.split(",")] if args.queries else default_ids
    suffix = "_compiled_validation" if args.validate_compiled else "_validation" if args.validate_worlds else "_pac_validation" if args.validate else ""
    if not query_ids or any(q < 1 or q > 99 for q in query_ids) or len(set(query_ids)) != len(query_ids):
        parser.error("Query numbers must be unique and between 1 and 99")
    output = pathlib.Path(config["out_dir"])
    manifest_path = output / "run_config.json"
    if manifest_path.exists() and json.loads(manifest_path.read_text()) != config:
        parser.error("Configuration changed: use a fresh out_dir to avoid mixing experiments")
    if not args.resume and any((output / f"q{q:02}{suffix}.csv").exists() for q in query_ids):
        parser.error("Results already exist: use a fresh out_dir or --resume")
    subprocess.run([args.runner, args.config, "--prepare"], check=True)
    manifest_path.write_text(json.dumps(config, indent=2) + "\n")
    summary = []
    runner_digest = digest_file(args.runner)
    for query in query_ids:
        name = f"q{query:02}"
        path = output / f"{name}{suffix}.csv"
        process_status = "completed"
        process_error = ""
        process_path = output / f"{name}{suffix}_process.json"
        inputs = (runner_digest + digest_file(pathlib.Path(config["queries_dir"]) / f"{name}.sql")
                  + digest_file(config.get("schema", "benchmark/tpcds/schema.sql")))
        hand_sql = handwritten_dir / f"{name}.sql"
        if not args.validate_compiled and hand_sql.exists():
            inputs += digest_file(hand_sql)
        inputs_digest = hashlib.sha256(inputs.encode()).hexdigest()
        state = json.loads(process_path.read_text()) if process_path.exists() else {}
        if args.resume and state and state.get("inputs_sha256") == inputs_digest:
            process_status, process_error = state["status"], state["error"]
        else:
            print(f"Running {name}", flush=True)
            with (output / f"{name}{suffix}.log").open("w") as log:
                try:
                    result = subprocess.run(
                        [args.runner, args.config, str(query)] + (["--validate-compiled"] if args.validate_compiled else ["--validate-worlds"] if args.validate_worlds else ["--validate"] if args.validate else []), stdout=log, stderr=subprocess.STDOUT,
                        timeout=config.get("timeout_seconds", 120),
                    )
                    if result.returncode:
                        process_status = "process_error"
                        process_error = f"exit code {result.returncode}; see {name}{suffix}.log"
                except subprocess.TimeoutExpired:
                    process_status = "timeout"
                    process_error = "Whole-query process timeout (both modes and all repetitions)"
            process_path.write_text(json.dumps(dict(status=process_status, error=process_error, inputs_sha256=inputs_digest)))
        rows = list(csv.DictReader(path.open())) if path.exists() else []
        if args.validate:
            if not args.validate_worlds:
                for row in rows:
                    row["method"] = "world_zero_diagnostic" if args.validate_compiled else "pac_equivalence"
            if rows:
                summary.extend(rows)
            else:
                summary.append(dict(query=name, method="validation", status=process_status if process_status != "completed" else "incomplete", error=process_error))
            fields = list(dict.fromkeys(key for row in summary for key in row))
            with (output / ("compiled_validation_summary.csv" if args.validate_compiled else "validation_summary.csv" if args.validate_worlds else "pac_validation_summary.csv")).open("w", newline="") as file:
                writer = csv.DictWriter(file, fieldnames=fields)
                writer.writeheader()
                writer.writerows(summary)
            print(f"{name}: " + (", ".join(f"{r['method']}={r['status']}" for r in rows) if rows else process_status + ": " + process_error), flush=True)
            continue
        for mode in ("duckdb", "simd_asa_handwritten", "simd_asa_auto", "naive_as"):
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
                world_multiplier=entries[-1].get("world_multiplier", "") if entries else "",
                median_ms=statistics.median(float(row["runtime_ms"]) for row in measured) if complete else "",
                empty_result=all(row["rows"] == "0" for row in measured) if complete else "",
                correctness="not_validated",
                error=entries[-1]["error"] if entries and entries[-1]["error"] else process_error,
            ))
        with (output / "summary.csv").open("w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=list(summary[0]))
            writer.writeheader()
            writer.writerows(summary)
        print(f"{name}: " + ", ".join(f"{r['mode']}={r['status']}" for r in summary[-4:]), flush=True)
    print(f"Results: {output / ('compiled_validation_summary.csv' if args.validate_compiled else 'validation_summary.csv' if args.validate_worlds else 'pac_validation_summary.csv' if args.validate else 'summary.csv')}")


if __name__ == "__main__":
    main()
