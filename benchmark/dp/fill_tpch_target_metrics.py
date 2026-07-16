#!/usr/bin/env python3
"""Fill mechanism-target metrics for selected stock TPC-H DP benchmark rows.

This is intentionally narrow: it reruns only the selected TPC-H rows, once with
noise enabled and once with the same rewrite but noise disabled, then compares
those two outputs.
"""

import argparse
import csv
import math
import os
import statistics
import subprocess
import sys
import textwrap
from typing import Dict, Iterable, List, Tuple


QUERIES = {
    "q01": (
        2,
        """\
        SELECT
            l_returnflag,
            l_linestatus,
            sum(l_quantity) AS sum_qty,
            sum(l_extendedprice) AS sum_base_price,
            sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
            sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
            avg(l_quantity) AS avg_qty,
            avg(l_extendedprice) AS avg_price,
            avg(l_discount) AS avg_disc,
            count(*) AS count_order
        FROM lineitem
        WHERE l_shipdate <= CAST('1998-09-02' AS date)
        GROUP BY l_returnflag, l_linestatus
        ORDER BY l_returnflag, l_linestatus
        """,
    ),
    "q05": (
        1,
        """\
        SELECT
            n_name,
            sum(l_extendedprice * (1 - l_discount)) AS revenue
        FROM customer, orders, lineitem, supplier, nation, region
        WHERE c_custkey = o_custkey
          AND l_orderkey = o_orderkey
          AND l_suppkey = s_suppkey
          AND c_nationkey = s_nationkey
          AND s_nationkey = n_nationkey
          AND n_regionkey = r_regionkey
          AND r_name = 'ASIA'
          AND o_orderdate >= CAST('1994-01-01' AS date)
          AND o_orderdate < CAST('1995-01-01' AS date)
        GROUP BY n_name
        ORDER BY revenue DESC
        """,
    ),
    "q06": (
        0,
        """\
        SELECT
            sum(l_extendedprice * l_discount) AS revenue
        FROM lineitem
        WHERE l_shipdate >= CAST('1994-01-01' AS date)
          AND l_shipdate < CAST('1995-01-01' AS date)
          AND l_discount BETWEEN 0.05 AND 0.07
          AND l_quantity < 24
        """,
    ),
    "q14": (
        0,
        """\
        SELECT
            100.00 * sum(CASE
                WHEN p_type LIKE 'PROMO%'
                THEN l_extendedprice * (1 - l_discount)
                ELSE 0
            END) / sum(l_extendedprice * (1 - l_discount)) AS promo_revenue
        FROM lineitem, part
        WHERE l_partkey = p_partkey
          AND l_shipdate >= CAST('1995-09-01' AS date)
          AND l_shipdate < CAST('1995-10-01' AS date)
        """,
    ),
    "q19": (
        0,
        """\
        SELECT
            sum(l_extendedprice * (1 - l_discount)) AS revenue
        FROM lineitem, part
        WHERE p_partkey = l_partkey
          AND l_shipmode IN ('AIR', 'AIR REG')
          AND l_shipinstruct = 'DELIVER IN PERSON'
          AND (
            (
              p_brand = 'Brand#12'
              AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
              AND l_quantity >= 1 AND l_quantity <= 11
              AND p_size BETWEEN 1 AND 5
            )
            OR (
              p_brand = 'Brand#23'
              AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
              AND l_quantity >= 10 AND l_quantity <= 20
              AND p_size BETWEEN 1 AND 10
            )
            OR (
              p_brand = 'Brand#34'
              AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
              AND l_quantity >= 20 AND l_quantity <= 30
              AND p_size BETWEEN 1 AND 15
            )
          )
        """,
    ),
}


TARGET_COLUMNS = [
    "target_utility",
    "target_recall",
    "target_precision",
    "target_median_error_pct",
    "target_q1_sum_median_error_pct",
    "target_q1_avg_median_error_pct",
    "target_q1_count_median_error_pct",
]


def sql_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def parse_float(value: str) -> float:
    if value is None or value == "":
        return math.nan
    return float(value)


def format_number(value: float) -> str:
    if value is None or not math.isfinite(value):
        return ""
    return f"{value:.12g}"


def csv_key(row: List[str], key_cols: int, row_index: int) -> str:
    if key_cols == 0:
        return f"#{row_index}"
    return "|".join(row[i] for i in range(key_cols))


def rows_by_key(rows: List[List[str]], key_cols: int, label: str) -> Dict[str, List[str]]:
    result = {}
    for idx, row in enumerate(rows):
        key = csv_key(row, key_cols, idx)
        if key in result:
            raise RuntimeError(f"{label} duplicate output key: {key}")
        result[key] = row
    return result


def numeric(value: str) -> float:
    if value == "":
        raise ValueError("NULL/empty value")
    value = value.strip()
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError("non-finite value")
    return parsed


def median(values: Iterable[float]) -> float:
    values = list(values)
    if not values:
        return math.nan
    return statistics.median(values)


def compare_rows(private_rows: List[List[str]], target_rows: List[List[str]], key_cols: int) -> Dict[str, str]:
    if not private_rows and not target_rows:
        return {
            "target_utility": "0",
            "target_recall": "1",
            "target_precision": "1",
            "target_median_error_pct": "0",
        }
    if private_rows and target_rows and len(private_rows[0]) != len(target_rows[0]):
        raise RuntimeError("private/target column-count mismatch")
    column_count = len(target_rows[0]) if target_rows else len(private_rows[0])
    if key_cols >= column_count:
        raise RuntimeError("no measure columns in comparison")

    private_by_key = rows_by_key(private_rows, key_cols, "private")
    target_by_key = rows_by_key(target_rows, key_cols, "target")
    measure_count = column_count - key_cols
    utility_sum = [0.0] * measure_count
    utility_count = [0] * measure_count
    cell_errors = []
    matched_rows = 0
    missing_rows = 0
    private_only_rows = 0

    for key, target_row in target_by_key.items():
        private_row = private_by_key.get(key)
        if private_row is None:
            missing_rows += 1
            continue
        if any(private_row[col] == "" for col in range(key_cols, column_count)):
            private_only_rows += 1
            continue
        matched_rows += 1
        for col in range(key_cols, column_count):
            try:
                private_value = numeric(private_row[col])
                target_value = numeric(target_row[col])
            except ValueError:
                continue
            abs_ref = max(0.00001, abs(target_value))
            error_pct = 100.0 * abs(private_value - target_value) / abs_ref
            metric_idx = col - key_cols
            utility_sum[metric_idx] += error_pct
            utility_count[metric_idx] += 1
            cell_errors.append(error_pct)

    for key in private_by_key:
        if key not in target_by_key:
            private_only_rows += 1

    recall = matched_rows / (matched_rows + missing_rows) if matched_rows + missing_rows else 1.0
    precision = matched_rows / (matched_rows + private_only_rows) if matched_rows + private_only_rows else 1.0
    per_column = [
        utility_sum[i] / utility_count[i]
        for i in range(measure_count)
        if utility_count[i] > 0
    ]
    utility = sum(per_column) / len(per_column) if per_column else 0.0
    median_error_pct = median(cell_errors)
    if not math.isfinite(median_error_pct):
        median_error_pct = 0.0
    return {
        "target_utility": format_number(utility),
        "target_recall": format_number(recall),
        "target_precision": format_number(precision),
        "target_median_error_pct": format_number(median_error_pct),
    }


def compare_q1_families(private_rows: List[List[str]], target_rows: List[List[str]], key_cols: int) -> Dict[str, str]:
    private_by_key = rows_by_key(private_rows, key_cols, "q1 private")
    target_by_key = rows_by_key(target_rows, key_cols, "q1 target")
    sum_errors = []
    avg_errors = []
    count_errors = []
    for key, target_row in target_by_key.items():
        private_row = private_by_key.get(key)
        if private_row is None:
            continue
        for col in range(key_cols, min(key_cols + 8, len(target_row))):
            if private_row[col] == "":
                continue
            try:
                private_value = numeric(private_row[col])
                target_value = numeric(target_row[col])
            except ValueError:
                continue
            error_pct = 100.0 * abs(private_value - target_value) / max(0.00001, abs(target_value))
            measure_idx = col - key_cols
            if measure_idx < 4:
                sum_errors.append(error_pct)
            elif measure_idx < 7:
                avg_errors.append(error_pct)
            else:
                count_errors.append(error_pct)
    return {
        "target_q1_sum_median_error_pct": format_number(median(sum_errors)),
        "target_q1_avg_median_error_pct": format_number(median(avg_errors)),
        "target_q1_count_median_error_pct": format_number(median(count_errors)),
    }


def setting_sql(
    row: Dict[str, str],
    noise: bool,
    query_sql: str,
    threads: int,
    memory_limit: str,
    extension_path: str,
) -> str:
    load_statement = f"LOAD {sql_quote(extension_path)}" if extension_path else "LOAD privacy"
    statements = [
        load_statement,
        "SET enable_progress_bar=false",
        f"SET threads={threads}",
        "SET privacy_diffcols=NULL",
        "SET priv_rewrite=true",
        "PRAGMA clear_privacy_metadata",
        "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey)",
        "ALTER TABLE customer SET PU",
        "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey)",
        "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey)",
        f"SET privacy_mode={sql_quote(row['mode'])}",
        f"SET dp_epsilon={row['epsilon']}",
        f"SET dp_delta={row['delta']}",
        f"SET dp_sum_bound={row['dp_sum_bound']}",
        "SET dp_sum_bounds=NULL",
        f"SET dp_count_bound={row['dp_count_bound']}",
        f"SET dp_max_groups_contributed={row['dp_max_groups_contributed']}",
        "SET dp_avg_lower_bounds=NULL",
        "SET dp_avg_upper_bounds=NULL",
        "SET dp_avg_lower_bound=NULL",
        "SET dp_avg_upper_bound=NULL",
        f"SET privacy_seed={row['seed']}",
        f"SET privacy_noise={'true' if noise else 'false'}",
    ]
    if memory_limit:
        statements.insert(2, f"SET memory_limit={sql_quote(memory_limit)}")
    if row.get("dp_sum_bounds"):
        statements.insert(16, f"SET dp_sum_bounds={sql_quote(row['dp_sum_bounds'])}")
    if row.get("dp_avg_lower_bounds") or row.get("dp_avg_upper_bounds"):
        statements.append(f"SET dp_avg_lower_bounds={sql_quote(row.get('dp_avg_lower_bounds', ''))}")
        statements.append(f"SET dp_avg_upper_bounds={sql_quote(row.get('dp_avg_upper_bounds', ''))}")
    statements.append(f"COPY ({textwrap.dedent(query_sql).strip()}) TO STDOUT (FORMAT CSV, HEADER TRUE)")
    return ";\n".join(statements) + ";\n"


def run_query(args: argparse.Namespace, row: Dict[str, str], noise: bool, query_sql: str) -> Tuple[List[str], List[List[str]]]:
    sql = setting_sql(row, noise, query_sql, args.threads, args.memory_limit, args.extension)
    db_path = row["db_path"]
    proc = subprocess.run(
        [args.duckdb, db_path],
        input=sql,
        text=True,
        capture_output=True,
        cwd=args.workdir,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip())
    reader = csv.reader(proc.stdout.splitlines())
    rows = list(reader)
    if not rows:
        raise RuntimeError("query produced no CSV output")
    return rows[0], rows[1:]


def row_matches(row: Dict[str, str], args: argparse.Namespace, modes: set, queries: set) -> bool:
    if row.get("success", "").lower() not in {"true", "t", "1"}:
        return False
    if row.get("dataset", "").lower() != args.dataset:
        return False
    if row.get("mode") not in modes:
        return False
    if row.get("query", "").lower() not in queries:
        return False
    delta = parse_float(row.get("delta", ""))
    if not math.isfinite(delta) or abs(delta - args.delta) > max(1e-15, abs(args.delta) * 1e-8):
        return False
    if args.bound_multipliers:
        multiplier = parse_float(row.get("bound_multiplier", ""))
        if all(abs(multiplier - value) > max(1e-15, abs(value) * 1e-8) for value in args.bound_multipliers):
            return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--extension", default="build/release/extension/privacy/privacy.duckdb_extension")
    parser.add_argument("--workdir", default=".")
    parser.add_argument("--dataset", default="tpch")
    parser.add_argument("--delta", type=float, default=1e-6)
    parser.add_argument("--modes", default="dp_standard,dp_elastic")
    parser.add_argument("--queries", default="q01,q05,q06,q14,q19")
    parser.add_argument("--bound-multipliers", default="")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--memory-limit", default="")
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    modes = {item.strip() for item in args.modes.split(",") if item.strip()}
    queries = {item.strip().lower() for item in args.queries.split(",") if item.strip()}
    args.bound_multipliers = [
        float(item.strip()) for item in args.bound_multipliers.split(",") if item.strip()
    ]

    with open(args.input, newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = list(reader.fieldnames or [])
        rows = list(reader)

    for column in TARGET_COLUMNS:
        if column not in fieldnames:
            fieldnames.append(column)
        for row in rows:
            row.setdefault(column, "")

    filled = 0
    for index, row in enumerate(rows):
        if not row_matches(row, args, modes, queries):
            continue
        query_name = row["query"].lower()
        if query_name not in QUERIES:
            raise RuntimeError(f"unsupported query for postprocess: {query_name}")
        key_cols, query_sql = QUERIES[query_name]
        private_header, private_rows = run_query(args, row, True, query_sql)
        target_header, target_rows = run_query(args, row, False, query_sql)
        if private_header != target_header:
            raise RuntimeError(f"{query_name} header mismatch: {private_header} vs {target_header}")
        metrics = compare_rows(private_rows, target_rows, key_cols)
        if query_name == "q01":
            metrics.update(compare_q1_families(private_rows, target_rows, key_cols))
        for column, value in metrics.items():
            row[column] = value
        filled += 1
        print(
            f"filled {filled}: row={index + 2} {row['dataset']} {row['mode']} {row['query']} "
            f"bound={row['bound_multiplier']} target_median={row['target_median_error_pct']}",
            flush=True,
        )
        if args.limit and filled >= args.limit:
            break

    with open(args.output, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {args.output}; filled {filled} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
