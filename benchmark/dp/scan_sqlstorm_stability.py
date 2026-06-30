#!/usr/bin/env python3
import argparse
import csv
import math
import re
import shutil
import sys
import threading
import time
from pathlib import Path

import duckdb


SO_TABLES = [
    "PostHistoryTypes",
    "CloseReasonTypes",
    "LinkTypes",
    "PostTypes",
    "VoteTypes",
    "PostHistory",
    "PostLinks",
    "Comments",
    "Badges",
    "Posts",
    "Users",
    "Votes",
    "Tags",
]

NUMERIC_TYPES = (
    "TINYINT",
    "SMALLINT",
    "INTEGER",
    "BIGINT",
    "HUGEINT",
    "UTINYINT",
    "USMALLINT",
    "UINTEGER",
    "UBIGINT",
    "UHUGEINT",
    "FLOAT",
    "DOUBLE",
    "DECIMAL",
    "REAL",
)


def clean_sql(text):
    sql = text.strip()
    while sql.endswith(";"):
        sql = sql[:-1].strip()
    return sql


def sql_quote_ident(identifier):
    return '"' + identifier.replace('"', '""') + '"'


def sql_string(value):
    return "'" + str(value).replace("'", "''") + "'"


def query_name(path):
    name = path.name
    if name.endswith(".sql"):
        name = name[:-4]
    return name


def shorten_error(err):
    text = " ".join(str(err).split())
    return text[:400]


def run_with_timeout(conn, sql, timeout_s):
    timer = None
    if timeout_s > 0:
        timer = threading.Timer(timeout_s, conn.interrupt)
        timer.daemon = True
        timer.start()
    try:
        return conn.execute(sql)
    finally:
        if timer is not None:
            timer.cancel()


def rewrite_tables(sql):
    rewritten = sql
    for table in sorted(SO_TABLES, key=len, reverse=True):
        pattern = re.compile(r"(?<![\w.])" + re.escape(table) + r"(?!\w)", re.IGNORECASE)
        rewritten = pattern.sub("lane_" + table, rewritten)
    return rewritten


def create_lane_views(conn, lanes):
    conn.execute("DROP TABLE IF EXISTS __so_current_lane")
    conn.execute("DROP TABLE IF EXISTS __so_user_lanes")
    conn.execute("CREATE TEMP TABLE __so_current_lane(lane INTEGER)")
    conn.execute("INSERT INTO __so_current_lane VALUES (0)")
    conn.execute(
        f"""
        CREATE TEMP TABLE __so_user_lanes AS
        SELECT Id,
               ((row_number() OVER (ORDER BY hash(Id), Id) - 1) % {lanes})::INTEGER AS lane
        FROM Users
        """
    )

    direct_user_tables = {
        "Users": ("Id", "Id"),
        "Badges": ("UserId", "Id"),
        "Comments": ("UserId", "Id"),
        "PostHistory": ("UserId", "Id"),
        "Votes": ("UserId", "Id"),
        "Posts": ("OwnerUserId", "Id"),
    }

    for table, (fk_col, lane_col) in direct_user_tables.items():
        conn.execute(f"DROP VIEW IF EXISTS lane_{table}")
        conn.execute(
            f"""
            CREATE TEMP VIEW lane_{table} AS
            SELECT t.*
            FROM {table} t
            JOIN __so_user_lanes l ON t.{fk_col} = l.{lane_col}
            JOIN __so_current_lane c ON l.lane = c.lane
            """
        )

    conn.execute("DROP VIEW IF EXISTS lane_PostLinks")
    conn.execute(
        """
        CREATE TEMP VIEW lane_PostLinks AS
        SELECT pl.*
        FROM PostLinks pl
        JOIN Posts p ON pl.PostId = p.Id
        JOIN __so_user_lanes l ON p.OwnerUserId = l.Id
        JOIN __so_current_lane c ON l.lane = c.lane
        """
    )

    for table in ["PostHistoryTypes", "CloseReasonTypes", "LinkTypes", "PostTypes", "VoteTypes", "Tags"]:
        conn.execute(f"DROP VIEW IF EXISTS lane_{table}")
        conn.execute(f"CREATE TEMP VIEW lane_{table} AS SELECT * FROM {table}")


def set_lane(conn, lane):
    conn.execute("DELETE FROM __so_current_lane")
    conn.execute("INSERT INTO __so_current_lane VALUES (?)", [lane])


def describe_query(conn, sql, timeout_s):
    result = run_with_timeout(conn, f"DESCRIBE SELECT * FROM ({sql}) q", timeout_s).fetchall()
    cols = []
    for row in result:
        cols.append((row[0], str(row[1]).upper()))
    return cols


def summary_sql(sql, cols):
    exprs = ["COUNT(*)::DOUBLE AS __row_count"]
    numeric_cols = []
    for idx, (name, typ) in enumerate(cols):
        if typ.startswith(NUMERIC_TYPES):
            alias = f"__sum_abs_{idx}"
            numeric_cols.append((name, typ, alias))
            exprs.append(
                f"SUM(COALESCE(ABS(TRY_CAST({sql_quote_ident(name)} AS DOUBLE)), 0.0))::DOUBLE AS {alias}"
            )
    return f"SELECT {', '.join(exprs)} FROM ({sql}) q", numeric_cols


def fetch_summary(conn, sql, cols, timeout_s):
    sql_text, numeric_cols = summary_sql(sql, cols)
    row = run_with_timeout(conn, sql_text, timeout_s).fetchone()
    values = {"__row_count": float(row[0]) if row and row[0] is not None else 0.0}
    for i, (_, _, alias) in enumerate(numeric_cols, start=1):
        values[alias] = float(row[i]) if row[i] is not None else 0.0
    return values, numeric_cols


def stats(values):
    if not values:
        return 0.0, 0.0, 0.0, 0.0
    mean = sum(values) / len(values)
    var = sum((v - mean) * (v - mean) for v in values) / len(values)
    stddev = math.sqrt(var)
    cv = stddev / abs(mean) if mean != 0 else (0.0 if stddev == 0 else math.inf)
    rel_range = (max(values) - min(values)) / abs(mean) if mean != 0 else (0.0 if max(values) == min(values) else math.inf)
    return mean, stddev, cv, rel_range


def compute_metric_stats(lane_values):
    metric_stats = {}
    max_cv = 0.0
    max_rel_range = 0.0
    max_stddev = 0.0
    for key, values in lane_values.items():
        mean, stddev, cv, rel_range = stats(values)
        metric_stats[key] = (mean, stddev, cv, rel_range)
        if math.isfinite(cv):
            max_cv = max(max_cv, cv)
        else:
            max_cv = math.inf
        if math.isfinite(rel_range):
            max_rel_range = max(max_rel_range, rel_range)
        else:
            max_rel_range = math.inf
        max_stddev = max(max_stddev, stddev)
    return metric_stats, max_stddev, max_cv, max_rel_range


def analyze_query(conn, path, lanes, timeout_s, lane_timeout_s, early_stop_lanes, early_stop_cv, early_stop_rel_range):
    sql = clean_sql(path.read_text())
    start = time.monotonic()
    try:
        cols = describe_query(conn, sql, timeout_s)
        full_summary, numeric_cols = fetch_summary(conn, sql, cols, timeout_s)
    except Exception as ex:
        return {
            "query": query_name(path),
            "path": str(path),
            "accepted": False,
            "stable": False,
            "error": shorten_error(ex),
            "elapsed_ms": int((time.monotonic() - start) * 1000),
        }

    lane_sql = rewrite_tables(sql)
    lane_values = {}
    lane_errors = []
    early_stopped = False
    for lane in range(lanes):
        try:
            set_lane(conn, lane)
            lane_summary, _ = fetch_summary(conn, lane_sql, cols, lane_timeout_s)
            for key, value in lane_summary.items():
                lane_values.setdefault(key, []).append(value)
            lanes_done = lane + 1
            if lanes_done >= early_stop_lanes and lanes_done < lanes:
                _, _, prefix_cv, prefix_rel_range = compute_metric_stats(lane_values)
                if prefix_cv > early_stop_cv or prefix_rel_range > early_stop_rel_range:
                    early_stopped = True
                    break
        except Exception as ex:
            lane_errors.append(f"lane {lane}: {shorten_error(ex)}")
            break

    accepted = True
    lanes_evaluated = max((len(v) for v in lane_values.values()), default=0)
    all_lanes_ok = len(lane_errors) == 0 and lanes_evaluated == lanes and all(len(v) == lanes for v in lane_values.values())
    max_cv = 0.0
    max_rel_range = 0.0
    max_stddev = 0.0
    metric_stats, max_stddev, max_cv, max_rel_range = compute_metric_stats(lane_values)

    row_mean, row_stddev, row_cv, row_rel_range = metric_stats.get("__row_count", (0.0, 0.0, math.inf, math.inf))
    stable = all_lanes_ok and max_cv <= 0.05 and max_rel_range <= 0.25 and row_mean > 0
    if early_stopped:
        lane_errors.append(
            f"early-stop after {lanes_evaluated} lanes: max_cv={max_cv:.6g}, "
            f"max_rel_range={max_rel_range:.6g}"
        )
    return {
        "query": query_name(path),
        "path": str(path),
        "accepted": accepted,
        "stable": stable,
        "error": "" if all_lanes_ok else "; ".join(lane_errors),
        "elapsed_ms": int((time.monotonic() - start) * 1000),
        "lanes_evaluated": lanes_evaluated,
        "columns": len(cols),
        "numeric_columns": len(numeric_cols),
        "full_rows": full_summary.get("__row_count", 0.0),
        "lane_row_mean": row_mean,
        "lane_row_stddev": row_stddev,
        "lane_row_cv": row_cv,
        "lane_row_rel_range": row_rel_range,
        "max_metric_stddev": max_stddev,
        "max_metric_cv": max_cv,
        "max_metric_rel_range": max_rel_range,
    }


def write_csv_header(writer):
    writer.writerow(
        [
            "query",
            "path",
            "accepted",
            "stable",
            "error",
            "elapsed_ms",
            "lanes_evaluated",
            "columns",
            "numeric_columns",
            "full_rows",
            "lane_row_mean",
            "lane_row_stddev",
            "lane_row_cv",
            "lane_row_rel_range",
            "max_metric_stddev",
            "max_metric_cv",
            "max_metric_rel_range",
        ]
    )


def write_row(writer, row):
    writer.writerow(
        [
            row.get("query", ""),
            row.get("path", ""),
            row.get("accepted", False),
            row.get("stable", False),
            row.get("error", ""),
            row.get("elapsed_ms", ""),
            row.get("lanes_evaluated", ""),
            row.get("columns", ""),
            row.get("numeric_columns", ""),
            row.get("full_rows", ""),
            row.get("lane_row_mean", ""),
            row.get("lane_row_stddev", ""),
            row.get("lane_row_cv", ""),
            row.get("lane_row_rel_range", ""),
            row.get("max_metric_stddev", ""),
            row.get("max_metric_cv", ""),
            row.get("max_metric_rel_range", ""),
        ]
    )


def main():
    parser = argparse.ArgumentParser(description="Scan SQLStorm StackOverflow queries for equal-lane stability.")
    parser.add_argument("--db", default="stackoverflow_dba_sqlstorm.db")
    parser.add_argument("--queries-dir", default="benchmark/sqlstorm/SQLStorm/v1.0/stackoverflow/queries")
    parser.add_argument("--out", default="benchmark/dp/sqlstorm_stackoverflow_stability.csv")
    parser.add_argument("--stable-dir", default="benchmark/dp/sqlstorm_stackoverflow_stable_queries")
    parser.add_argument("--lanes", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--lane-timeout", type=float, default=0.5)
    parser.add_argument("--early-stop-lanes", type=int, default=8)
    parser.add_argument("--early-stop-cv", type=float, default=0.25)
    parser.add_argument("--early-stop-rel-range", type=float, default=1.0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--progress-every", type=int, default=100)
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

    conn = duckdb.connect(args.db)
    conn.execute("PRAGMA threads=1")
    conn.execute("PRAGMA memory_limit='8GB'")
    create_lane_views(conn, args.lanes)

    counts = {"accepted": 0, "stable": 0, "failed": 0}
    with out_path.open("w", newline="") as f:
        writer = csv.writer(f)
        write_csv_header(writer)
        for i, path in enumerate(query_paths, start=1):
            row = analyze_query(
                conn,
                path,
                args.lanes,
                args.timeout,
                args.lane_timeout,
                args.early_stop_lanes,
                args.early_stop_cv,
                args.early_stop_rel_range,
            )
            write_row(writer, row)
            f.flush()
            if row["accepted"]:
                counts["accepted"] += 1
            else:
                counts["failed"] += 1
            if row["stable"]:
                counts["stable"] += 1
                shutil.copy2(path, stable_dir / path.name)
            if i % args.progress_every == 0 or i == len(query_paths):
                print(
                    f"[{i}/{len(query_paths)}] accepted={counts['accepted']} "
                    f"stable={counts['stable']} failed={counts['failed']}",
                    flush=True,
                )

    print(f"wrote {out_path}")
    print(f"stable queries copied to {stable_dir}")
    print(f"accepted={counts['accepted']} stable={counts['stable']} failed={counts['failed']}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
