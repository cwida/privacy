#!/usr/bin/env python3
import argparse
import csv
import shutil
import subprocess
import sys
import time
from pathlib import Path


STABILITY_COLUMNS = [
    "group_row",
    "aggregate_index",
    "aggregate_name",
    "group_key_json",
    "lane_count",
    "valid_count",
    "null_count",
    "mean",
    "stddev_pop",
    "cv",
    "min",
    "median",
    "max",
    "range",
    "mad",
    "max_abs_dev",
    "stable_stddev_1",
    "stable_mad_1",
]


def clean_sql(text):
    sql = text.strip()
    while sql.endswith(";"):
        sql = sql[:-1].strip()
    return sql


def sql_string(value):
    return "'" + value.replace("'", "''") + "'"


def query_name(path):
    return path.stem


def shorten_error(text):
    return " ".join(text.split())[:800]


def bool_text(value):
    return "true" if value else "false"


def build_sql(user_sql, args):
    return f"""
LOAD privacy;
SET threads = 1;
SET privacy_mode = 'dp_sass';
SET privacy_noise = false;
SET dp_epsilon = {args.epsilon};
SET dp_delta = {args.delta};
SET dp_sample_lanes = {args.sample_lanes};
SET dp_sass_exact_balanced_lanes = {bool_text(args.exact_balanced_lanes)};
SET dp_count_bound = {args.count_bound};
SET dp_sum_bound = {args.sum_bound};
SET dp_max_groups_contributed = {args.cu};
SET dp_sass_count_output_bound = {args.count_output_bound};
SET dp_sass_sum_output_bound = {args.sum_output_bound};
SET dp_sass_avg_lower_bound = {args.avg_lower_bound};
SET dp_sass_avg_upper_bound = {args.avg_upper_bound};
SET dp_sass_minmax_lower_bound = {args.minmax_lower_bound};
SET dp_sass_minmax_upper_bound = {args.minmax_upper_bound};
SELECT group_row, aggregate_index, aggregate_name, group_key_json,
       lane_count, valid_count, null_count, mean, stddev_pop, cv,
       min, median, max, range, mad, max_abs_dev,
       stable_stddev_1, stable_mad_1
FROM dp_sass_stability_query({sql_string(user_sql)});
"""


def run_query(path, args):
    sql = clean_sql(path.read_text())
    cmd = [args.duckdb, args.db, "-csv", "-noheader", "-c", build_sql(sql, args)]
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return False, [], "timeout", int((time.monotonic() - start) * 1000)
    elapsed_ms = int((time.monotonic() - start) * 1000)
    if proc.returncode != 0:
        return False, [], shorten_error(proc.stderr or proc.stdout), elapsed_ms
    rows = []
    reader = csv.reader(proc.stdout.splitlines())
    for row in reader:
        if not row:
            continue
        if len(row) != len(STABILITY_COLUMNS):
            return False, [], f"unexpected output row with {len(row)} columns: {row[:4]}", elapsed_ms
        rows.append(dict(zip(STABILITY_COLUMNS, row)))
    if not rows:
        return False, [], "no stability rows", elapsed_ms
    return True, rows, "", elapsed_ms


def write_header(writer):
    writer.writerow(
        [
            "query",
            "path",
            "accepted",
            "stable_stddev_all",
            "stable_mad_all",
            "error",
            "elapsed_ms",
            "epsilon",
            "delta",
            "sample_lanes",
            "exact_balanced_lanes",
            "count_bound",
            "sum_bound",
            "cu",
            "count_output_bound",
            "sum_output_bound",
            "avg_lower_bound",
            "avg_upper_bound",
            "minmax_lower_bound",
            "minmax_upper_bound",
        ]
        + STABILITY_COLUMNS
    )


def write_failure(writer, path, args, error, elapsed_ms):
    writer.writerow(
        [
            query_name(path),
            str(path),
            "false",
            "false",
            "false",
            error,
            elapsed_ms,
            args.epsilon,
            args.delta,
            args.sample_lanes,
            bool_text(args.exact_balanced_lanes),
            args.count_bound,
            args.sum_bound,
            args.cu,
            args.count_output_bound,
            args.sum_output_bound,
            args.avg_lower_bound,
            args.avg_upper_bound,
            args.minmax_lower_bound,
            args.minmax_upper_bound,
        ]
        + [""] * len(STABILITY_COLUMNS)
    )


def write_success(writer, path, args, rows, elapsed_ms):
    stable_stddev_all = all(row["stable_stddev_1"].lower() == "true" for row in rows)
    stable_mad_all = all(row["stable_mad_1"].lower() == "true" for row in rows)
    prefix = [
        query_name(path),
        str(path),
        "true",
        bool_text(stable_stddev_all),
        bool_text(stable_mad_all),
        "",
        elapsed_ms,
        args.epsilon,
        args.delta,
        args.sample_lanes,
        bool_text(args.exact_balanced_lanes),
        args.count_bound,
        args.sum_bound,
        args.cu,
        args.count_output_bound,
        args.sum_output_bound,
        args.avg_lower_bound,
        args.avg_upper_bound,
        args.minmax_lower_bound,
        args.minmax_upper_bound,
    ]
    for row in rows:
        writer.writerow(prefix + [row[col] for col in STABILITY_COLUMNS])
    return stable_stddev_all, stable_mad_all


def main():
    parser = argparse.ArgumentParser(description="Scan SQLStorm StackOverflow queries using dp_sass_stability_query.")
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--db", default="stackoverflow_dba_sqlstorm.db")
    parser.add_argument("--queries-dir", default="benchmark/sqlstorm/SQLStorm/v1.0/stackoverflow/queries")
    parser.add_argument("--out", default="benchmark/dp/sqlstorm_stackoverflow_sass_stability.csv")
    parser.add_argument("--stable-dir", default="benchmark/dp/sqlstorm_stackoverflow_sass_stable_queries")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--progress-every", type=int, default=100)
    parser.add_argument("--epsilon", type=float, default=1.0)
    parser.add_argument("--delta", type=float, default=1e-6)
    parser.add_argument("--sample-lanes", type=int, default=1)
    parser.add_argument("--exact-balanced-lanes", action="store_true")
    parser.add_argument("--count-bound", type=float, default=100000.0)
    parser.add_argument("--sum-bound", type=float, default=1000000000.0)
    parser.add_argument("--cu", type=float, default=100.0)
    parser.add_argument("--count-output-bound", type=float, default=1000000000.0)
    parser.add_argument("--sum-output-bound", type=float, default=1000000000000.0)
    parser.add_argument("--avg-lower-bound", type=float, default=-1000000000.0)
    parser.add_argument("--avg-upper-bound", type=float, default=1000000000.0)
    parser.add_argument("--minmax-lower-bound", type=float, default=-1000000000.0)
    parser.add_argument("--minmax-upper-bound", type=float, default=1000000000.0)
    args = parser.parse_args()

    query_paths = sorted(Path(args.queries_dir).glob("*.sql"))
    if args.limit > 0:
        query_paths = query_paths[: args.limit]
    if not query_paths:
        raise RuntimeError(f"no .sql files found in {args.queries_dir}")

    out_path = Path(args.out)
    stable_dir = Path(args.stable_dir)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    stable_dir.mkdir(parents=True, exist_ok=True)
    for old_sql in stable_dir.glob("*.sql"):
        old_sql.unlink()

    counts = {"accepted": 0, "stable_stddev": 0, "stable_mad": 0, "failed": 0}
    with out_path.open("w", newline="") as f:
        writer = csv.writer(f)
        write_header(writer)
        for i, path in enumerate(query_paths, start=1):
            accepted, rows, error, elapsed_ms = run_query(path, args)
            if accepted:
                counts["accepted"] += 1
                stable_stddev, stable_mad = write_success(writer, path, args, rows, elapsed_ms)
                if stable_stddev:
                    counts["stable_stddev"] += 1
                if stable_mad:
                    counts["stable_mad"] += 1
                    shutil.copy2(path, stable_dir / path.name)
            else:
                counts["failed"] += 1
                write_failure(writer, path, args, error, elapsed_ms)
            f.flush()
            if i % args.progress_every == 0 or i == len(query_paths):
                print(
                    f"[{i}/{len(query_paths)}] accepted={counts['accepted']} "
                    f"stable_stddev={counts['stable_stddev']} stable_mad={counts['stable_mad']} "
                    f"failed={counts['failed']}",
                    flush=True,
                )
    print(f"wrote {out_path}")
    print(f"stable MAD queries copied to {stable_dir}")
    print(counts)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
