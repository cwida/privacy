#!/usr/bin/env python3
import argparse
import csv
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

TPCH_STOCK_DP_QUERIES = {
    "q01": """
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
    "q05": """
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
    "q06": """
SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= CAST('1994-01-01' AS date)
  AND l_shipdate < CAST('1995-01-01' AS date)
  AND l_discount BETWEEN 0.05 AND 0.07
  AND l_quantity < 24
""",
    "q14": """
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
    "q19": """
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
}

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

META_COLUMNS = [
    "dataset",
    "workload",
    "sf",
    "db",
    "query",
    "bound_multiplier",
    "count_bound",
    "sum_bound",
    "c_u",
    "sass_count_output_bound",
    "sass_sum_output_bound",
    "sass_m",
    "sass_rescale",
    "elapsed_ms",
    "success",
    "error",
]

SUMMARY_COLUMNS = [
    "dataset",
    "workload",
    "sf",
    "db",
    "query",
    "bound_multiplier",
    "count_bound",
    "sum_bound",
    "c_u",
    "sass_count_output_bound",
    "sass_sum_output_bound",
    "sass_m",
    "sass_rescale",
    "success",
    "aggregate_rows",
    "elapsed_ms",
    "mean_cv",
    "median_cv",
    "max_cv",
    "mean_stddev_pop",
    "max_stddev_pop",
    "mean_mad",
    "max_mad",
    "mean_range",
    "max_range",
    "min_valid_count",
    "max_valid_count",
    "error",
]


def sql_string(value):
    return "'" + value.replace("'", "''") + "'"


def format_number(value):
    return "{:.17g}".format(float(value))


def shorten_error(text):
    return " ".join(text.split())[:800]


def first_value(config, key, default):
    values = config.get(key)
    if not values:
        return default
    return values[0]


def config_list(config, plural, singular, default):
    if plural in config:
        value = config[plural]
        return value if isinstance(value, list) else [value]
    if singular in config:
        return [config[singular]]
    return default


def numeric(value):
    if value == "" or value is None:
        return None
    return float(value)


def bool_text(value):
    return "true" if value else "false"


def build_sql(query_sql, point):
    return f"""
LOAD privacy;
SET threads = {point["threads"]};
PRAGMA clear_privacy_metadata;
ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);
ALTER TABLE customer SET PU;
ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);
ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);
SET priv_rewrite = true;
SET privacy_diffcols = NULL;
SET privacy_mode = 'dp_sass';
SET privacy_noise = false;
SET dp_epsilon = {format_number(point["epsilon"])};
SET dp_delta = {format_number(point["delta"])};
SET dp_sample_lanes = {point["sample_lanes"]};
SET dp_sass_m = {point["sass_m"]};
SET dp_sass_rescale = {bool_text(point["sass_rescale"])};
SET dp_sass_release = 'average';
SET dp_count_bound = {format_number(point["count_bound"])};
SET dp_sum_bound = {format_number(point["sum_bound"])};
SET dp_max_groups_contributed = {format_number(point["c_u"])};
SET dp_sass_count_output_bound = {format_number(point["sass_count_output_bound"])};
SET dp_sass_sum_output_bound = {format_number(point["sass_sum_output_bound"])};
SET dp_sass_avg_lower_bound = 0;
SET dp_sass_avg_upper_bound = {format_number(point["sum_bound"])};
SET dp_sass_minmax_lower_bound = 0;
SET dp_sass_minmax_upper_bound = {format_number(point["sum_bound"])};
SELECT group_row, aggregate_index, aggregate_name, group_key_json,
       lane_count, valid_count, null_count, mean, stddev_pop, cv,
       min, median, max, range, mad, max_abs_dev,
       stable_stddev_1, stable_mad_1
FROM dp_sass_stability_query({sql_string(query_sql.strip())});
"""


def run_point(args, point, query_sql):
    cmd = [
        str(args.duckdb),
        point["db"],
        "-csv",
        "-noheader",
        "-c",
        build_sql(query_sql, point),
    ]
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        elapsed_ms = int((time.monotonic() - start) * 1000)
        return False, [], "timeout", elapsed_ms

    elapsed_ms = int((time.monotonic() - start) * 1000)
    if proc.returncode != 0:
        return False, [], shorten_error(proc.stderr or proc.stdout), elapsed_ms

    rows = []
    for row in csv.reader(proc.stdout.splitlines()):
        if not row or row == ["Success"] or row == ["success"]:
            continue
        if len(row) != len(STABILITY_COLUMNS):
            return False, [], f"unexpected CSV row with {len(row)} columns: {row[:4]}", elapsed_ms
        rows.append(dict(zip(STABILITY_COLUMNS, row)))
    if not rows:
        return False, [], "no stability rows", elapsed_ms
    return True, rows, "", elapsed_ms


def summarize_rows(meta, rows, success, error):
    values = {key: [] for key in ["cv", "stddev_pop", "mad", "range", "valid_count"]}
    for row in rows:
        for key in values:
            value = numeric(row[key])
            if value is not None:
                values[key].append(value)

    def mean(key):
        return statistics.fmean(values[key]) if values[key] else ""

    def median(key):
        return statistics.median(values[key]) if values[key] else ""

    def max_value(key):
        return max(values[key]) if values[key] else ""

    def min_value(key):
        return min(values[key]) if values[key] else ""

    return {
        **{key: meta[key] for key in SUMMARY_COLUMNS if key in meta},
        "success": bool_text(success),
        "aggregate_rows": len(rows),
        "mean_cv": mean("cv"),
        "median_cv": median("cv"),
        "max_cv": max_value("cv"),
        "mean_stddev_pop": mean("stddev_pop"),
        "max_stddev_pop": max_value("stddev_pop"),
        "mean_mad": mean("mad"),
        "max_mad": max_value("mad"),
        "mean_range": mean("range"),
        "max_range": max_value("range"),
        "min_valid_count": min_value("valid_count"),
        "max_valid_count": max_value("valid_count"),
        "error": error,
    }


def load_points(config, args):
    dataset_filter = set(args.datasets.split(",")) if args.datasets else None
    query_filter = set(args.queries.split(",")) if args.queries else None
    multipliers = config.get("bound_multipliers", [1.0])
    threads = args.threads if args.threads is not None else config.get("threads", 1)
    epsilon = first_value(config, "epsilons", 1.0)
    delta = first_value(config, "deltas", 1e-6)
    sample_lanes = first_value(config, "sample_lanes", 1)
    sass_m_values = config_list(config, "sass_ms", "sass_m", [64])
    sass_rescale_values = config_list(config, "sass_rescales", "sass_rescale", [True])

    points = []
    for dataset in config["datasets"]:
        if dataset_filter and dataset["name"] not in dataset_filter:
            continue
        dataset_sass_m_values = config_list(dataset, "sass_ms", "sass_m", sass_m_values)
        dataset_sass_rescale_values = config_list(dataset, "sass_rescales", "sass_rescale", sass_rescale_values)
        for query in dataset.get("query_names", []):
            if query_filter and query not in query_filter:
                continue
            if query not in TPCH_STOCK_DP_QUERIES:
                raise ValueError(f"no built-in SQL for query {query}")
            for multiplier in multipliers:
                count_bound = first_value(dataset, "count_bounds", 1.0) * multiplier
                sum_bound = first_value(dataset, "sum_bounds", 1.0) * multiplier
                for sass_m in dataset_sass_m_values:
                    for sass_rescale in dataset_sass_rescale_values:
                        points.append(
                            {
                                "dataset": dataset["name"],
                                "workload": dataset.get("workload", ""),
                                "sf": dataset.get("sf", ""),
                                "db": dataset["db"],
                                "query": query,
                                "bound_multiplier": multiplier,
                                "count_bound": count_bound,
                                "sum_bound": sum_bound,
                                "c_u": first_value(dataset, "c_u", 1.0),
                                "sass_count_output_bound": first_value(dataset, "sass_count_output_bounds", 1.0),
                                "sass_sum_output_bound": first_value(dataset, "sass_sum_output_bounds", 1.0),
                                "sass_m": sass_m,
                                "sass_rescale": sass_rescale,
                                "threads": threads,
                                "epsilon": epsilon,
                                "delta": delta,
                                "sample_lanes": sample_lanes,
                            }
                        )
    return points


def main():
    parser = argparse.ArgumentParser(description="Run SASS lane-stability analysis for the TPCH/JCC-H DP queries.")
    parser.add_argument("--config", default="benchmark/dp/configs/tpch_jcch_sf30_all5_full_runtime_3run.json")
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--out", default="benchmark/dp/tpch_jcch_sf30_all5_sass_stability.csv")
    parser.add_argument("--summary-out", default="benchmark/dp/tpch_jcch_sf30_all5_sass_stability_summary.csv")
    parser.add_argument("--datasets", help="comma-separated dataset names, e.g. tpch,jcch")
    parser.add_argument("--queries", help="comma-separated query names, e.g. q01,q06")
    parser.add_argument("--threads", type=int)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--strict", action="store_true", help="fail when a configured database is missing")
    args = parser.parse_args()

    config_path = ROOT / args.config
    config = json.loads(config_path.read_text())
    args.duckdb = ROOT / args.duckdb if not Path(args.duckdb).is_absolute() else Path(args.duckdb)
    out_path = ROOT / args.out
    summary_path = ROOT / args.summary_out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    points = load_points(config, args)
    if not points:
        print("no matching points", file=sys.stderr)
        return 1

    with out_path.open("w", newline="") as raw_file, summary_path.open("w", newline="") as summary_file:
        raw_writer = csv.DictWriter(raw_file, fieldnames=META_COLUMNS + STABILITY_COLUMNS)
        summary_writer = csv.DictWriter(summary_file, fieldnames=SUMMARY_COLUMNS)
        raw_writer.writeheader()
        summary_writer.writeheader()

        for index, point in enumerate(points, start=1):
            db_path = ROOT / point["db"] if not Path(point["db"]).is_absolute() else Path(point["db"])
            point["db"] = str(db_path)
            label = f'{point["dataset"]} {point["query"]} x{point["bound_multiplier"]:g} m={point["sass_m"]}'
            print(f"[{index}/{len(points)}] {label}", flush=True)
            if not db_path.exists():
                error = f"missing database: {db_path}"
                if args.strict:
                    raise FileNotFoundError(error)
                meta = {key: point.get(key, "") for key in META_COLUMNS}
                meta.update({"elapsed_ms": 0, "success": "false", "error": error})
                raw_writer.writerow({**meta, **{key: "" for key in STABILITY_COLUMNS}})
                summary_writer.writerow(summarize_rows(meta, [], False, error))
                continue

            success, rows, error, elapsed_ms = run_point(args, point, TPCH_STOCK_DP_QUERIES[point["query"]])
            meta = {key: point.get(key, "") for key in META_COLUMNS}
            meta.update({"elapsed_ms": elapsed_ms, "success": bool_text(success), "error": error})
            if success:
                for row in rows:
                    raw_writer.writerow({**meta, **row})
            else:
                raw_writer.writerow({**meta, **{key: "" for key in STABILITY_COLUMNS}})
            summary_writer.writerow(summarize_rows(meta, rows, success, error))

    print(f"wrote {out_path}")
    print(f"wrote {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
