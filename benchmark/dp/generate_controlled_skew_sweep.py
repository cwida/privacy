#!/usr/bin/env python3
import argparse
import csv
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TEMPLATE = ROOT / "benchmark/dp/configs/tpch_jcch_sf30_all5_full_runtime_3run.json"
DEFAULT_OUT_DIR = ROOT / "benchmark/dp/skew_sweep"
QUERY_ORDER = ["q01", "q05", "q06", "q14", "q19"]


def parse_int_list(text):
    return [int(value.strip()) for value in text.split(",") if value.strip()]


def relpath(path):
    return str(Path(path).resolve().relative_to(ROOT))


def run(cmd, cwd=ROOT, timeout=None):
    start = time.monotonic()
    print("[skew] " + " ".join(str(part) for part in cmd), flush=True)
    proc = subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True, timeout=timeout)
    if proc.returncode != 0:
        if proc.stdout:
            print(proc.stdout, file=sys.stderr)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        raise RuntimeError(f"command failed with exit code {proc.returncode}: {' '.join(str(part) for part in cmd)}")
    elapsed = time.monotonic() - start
    print(f"[skew] done in {elapsed:.1f}s", flush=True)
    return proc.stdout


def run_stream(cmd, cwd=ROOT, timeout=None):
    start = time.monotonic()
    print("[skew] " + " ".join(str(part) for part in cmd), flush=True)
    proc = subprocess.run(cmd, cwd=str(cwd), text=True, timeout=timeout)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed with exit code {proc.returncode}: {' '.join(str(part) for part in cmd)}")
    elapsed = time.monotonic() - start
    print(f"[skew] done in {elapsed:.1f}s", flush=True)


def sql_quote(path):
    return "'" + str(path).replace("'", "''") + "'"


def run_duckdb(duckdb, db_path, sql, csv_output=False, timeout=3600):
    cmd = [str(duckdb), str(db_path)]
    if csv_output:
        cmd.extend(["-csv", "-noheader"])
    cmd.extend(["-c", sql])
    return run(cmd, timeout=timeout)


def copy_db(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        run(["cp", "--reflink=auto", str(src), str(dst)], timeout=3600)
    except Exception:
        shutil.copy2(src, dst)


def collect_db_stats(duckdb, db_path):
    sql = """
SET priv_rewrite = false;
SELECT
    (SELECT COUNT(*) FROM orders) AS order_rows,
    (SELECT COUNT(DISTINCT o_custkey) FROM orders) AS distinct_order_customers,
    (SELECT MAX(cnt) FROM (SELECT COUNT(*) AS cnt FROM orders GROUP BY o_custkey)) AS max_orders_per_customer,
    (SELECT quantile_cont(cnt, 0.99) FROM (SELECT COUNT(*) AS cnt FROM orders GROUP BY o_custkey)) AS p99_orders_per_customer;
"""
    output = run_duckdb(duckdb, db_path, sql, csv_output=True)
    rows = [row for row in csv.reader(output.splitlines()) if row and row != ["Success"] and row != ["success"]]
    if len(rows) != 1 or len(rows[0]) != 4:
        raise RuntimeError(f"unexpected stats output for {db_path}: {rows}")
    return {
        "order_rows": rows[0][0],
        "distinct_order_customers": rows[0][1],
        "max_orders_per_customer": rows[0][2],
        "p99_orders_per_customer": rows[0][3],
    }


def validate_db(duckdb, db_path):
    sql = """
SET priv_rewrite = false;
SELECT COUNT(*)
FROM orders o
LEFT JOIN customer c ON o.o_custkey = c.c_custkey
WHERE c.c_custkey IS NULL;
"""
    output = run_duckdb(duckdb, db_path, sql, csv_output=True)
    rows = [row for row in csv.reader(output.splitlines()) if row and row != ["Success"] and row != ["success"]]
    if len(rows) != 1 or rows[0][0] != "0":
        raise RuntimeError(f"{db_path} has invalid o_custkey references: {rows}")


def apply_privacy_metadata(duckdb, db_path):
    sql = """
LOAD privacy;
PRAGMA clear_privacy_metadata;
ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);
ALTER TABLE customer SET PU;
ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);
ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);
CHECKPOINT;
"""
    run_duckdb(duckdb, db_path, sql)


def skew_db(duckdb, base_db, dst_db, remap_percent, n_whales, threads, force):
    if dst_db.exists() and not force:
        print(f"[skew] reusing {dst_db}", flush=True)
    else:
        if dst_db.exists():
            dst_db.unlink()
        copy_db(base_db, dst_db)
        sql = f"""
SET threads = {threads};
UPDATE orders
SET o_custkey = 1 + (o_orderkey % {n_whales})
WHERE o_orderkey % 100 < {remap_percent};
CHECKPOINT;
"""
        run_duckdb(duckdb, dst_db, sql, timeout=7200)
        apply_privacy_metadata(duckdb, dst_db)
    validate_db(duckdb, dst_db)


def load_template_entries(template_path):
    config = json.loads(template_path.read_text())
    by_query = {}
    for dataset in config["datasets"]:
        dataset_name = dataset["name"]
        if dataset_name not in {"tpch", "jcch"}:
            continue
        for query in dataset.get("query_names", []):
            by_query[(dataset_name, query)] = dataset
    for dataset_name in ("tpch", "jcch"):
        missing = [query for query in QUERY_ORDER if (dataset_name, query) not in by_query]
        if missing:
            raise RuntimeError(f"template is missing {dataset_name} entries for: {', '.join(missing)}")
    return config, by_query


def make_config(template_config, by_query, variants, config_path, results_path, runs, threads, query_names):
    datasets = []
    for variant in variants:
        for query in query_names:
            dataset_name = "tpch" if variant["variant"] == "baseline" else "jcch"
            entry = dict(by_query[(dataset_name, query)])
            entry["name"] = dataset_name
            entry["workload"] = "tpch_stock_dp" if dataset_name == "tpch" else "jcch_stock_dp"
            entry["db"] = variant["db"]
            entry["query_names"] = [query]
            entry["modes"] = (
                ["duckdb", "dp_standard", "dp_elastic", "dp_sass"]
                if variant["variant"] == "baseline"
                else ["dp_standard", "dp_elastic", "dp_sass"]
            )
            datasets.append(entry)

    config = {
        "out": relpath(results_path),
        "runs": runs,
        "threads": threads,
        "modes": ["duckdb", "dp_standard", "dp_elastic", "dp_sass"],
        "epsilons": template_config.get("epsilons", [1.0]),
        "deltas": template_config.get("deltas", [2.2222222222222222e-7]),
		"bound_multipliers": template_config.get("bound_multipliers", [1.0]),
		"sample_lanes": template_config.get("sample_lanes", [1]),
		"sass_ms": template_config.get("sass_ms", [64]),
		"sass_releases": ["median", "average"],
		"datasets": datasets,
	}
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(json.dumps(config, indent=2) + "\n")


def write_metadata(path, rows):
    fieldnames = [
        "variant",
        "db",
        "sf",
        "remap_percent",
        "whales_per_sf",
        "n_whales",
        "order_rows",
        "distinct_order_customers",
        "max_orders_per_customer",
        "p99_orders_per_customer",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})


def append_csv(src_path, dst_path):
    if not src_path.exists():
        raise FileNotFoundError(src_path)
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    write_header = not dst_path.exists() or dst_path.stat().st_size == 0
    with src_path.open(newline="") as src, dst_path.open("a", newline="") as dst:
        reader = csv.reader(src)
        writer = csv.writer(dst)
        header = next(reader, None)
        if header is None:
            return
        if write_header:
            writer.writerow(header)
        for row in reader:
            writer.writerow(row)


def run_config(runner, config_path):
    run_stream([str(runner), "--config", str(config_path)], timeout=None)


def remove_skew_db(db_path):
    if db_path.exists():
        print(f"[skew] removing {db_path}", flush=True)
        db_path.unlink()


def sequential_config_paths(out_dir, sf, variant, runs):
    prefix = out_dir / f"controlled_skew_sf{sf}_{variant}_sequential_{runs}run"
    return prefix.with_suffix(".json"), Path(str(prefix) + "_results.csv")


def run_sequential(args, duckdb, runner, base_db, template_config, by_query, remap_percents, whales_per_sf_values):
    out_dir = Path(args.out_dir)
    rows = []
    metadata_path = out_dir / f"controlled_skew_sf{args.sf}_metadata.csv"
    combined_results = out_dir / f"controlled_skew_sf{args.sf}_sequential_{args.runs}run_results.csv"
    if combined_results.exists():
        combined_results.unlink()

    baseline = {
        "variant": "baseline",
        "db": relpath(base_db),
        "sf": args.sf,
        "remap_percent": 0,
        "whales_per_sf": "",
        "n_whales": "",
    }
    baseline.update(collect_db_stats(duckdb, base_db))
    rows.append(baseline)
    write_metadata(metadata_path, rows)
    config_path, results_path = sequential_config_paths(out_dir, args.sf, "baseline", args.runs)
    make_config(template_config, by_query, [baseline], config_path, results_path, args.runs, args.threads, QUERY_ORDER)
    run_config(runner, config_path)
    append_csv(results_path, combined_results)

    for remap_percent in remap_percents:
        for whales_per_sf in whales_per_sf_values:
            n_whales = whales_per_sf * args.sf
            variant = f"r{remap_percent}_w{whales_per_sf}"
            db_path = out_dir / f"controlled_skew_sf{args.sf}_{variant}.db"
            row = {
                "variant": variant,
                "db": relpath(db_path),
                "sf": args.sf,
                "remap_percent": remap_percent,
                "whales_per_sf": whales_per_sf,
                "n_whales": n_whales,
            }
            try:
                skew_db(duckdb, base_db, db_path, remap_percent, n_whales, args.generate_threads, args.force)
                row.update(collect_db_stats(duckdb, db_path))
                rows.append(row)
                write_metadata(metadata_path, rows)
                config_path, results_path = sequential_config_paths(out_dir, args.sf, variant, args.runs)
                make_config(template_config, by_query, [row], config_path, results_path, args.runs, args.threads,
                            QUERY_ORDER)
                run_config(runner, config_path)
                append_csv(results_path, combined_results)
            finally:
                if not args.keep_skew_dbs:
                    remove_skew_db(db_path)

    print(f"[skew] wrote {metadata_path}")
    print(f"[skew] wrote {combined_results}")


def main():
    parser = argparse.ArgumentParser(description="Generate controlled SF30 order-skew variants and DP configs.")
    parser.add_argument("--duckdb", default=str(ROOT / "build/release/duckdb"))
    parser.add_argument("--runner", default=str(ROOT / "build/release/extension/privacy/dp_benchmark_runner"))
    parser.add_argument("--base-db", default=str(ROOT / "tpch_sf30_graviton.db"))
    parser.add_argument("--template", default=str(DEFAULT_TEMPLATE))
    parser.add_argument("--out-dir", default=str(DEFAULT_OUT_DIR))
    parser.add_argument("--sf", type=int, default=30)
    parser.add_argument("--remap-percents", default="10,30,60")
    parser.add_argument("--whales-per-sf", default="50,150,450")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--smoke-runs", type=int, default=1)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--generate-threads", type=int, default=16)
    parser.add_argument("--smoke-query", default="q06")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--run-sequential", action="store_true")
    parser.add_argument("--keep-skew-dbs", action="store_true")
    args = parser.parse_args()

    duckdb = Path(args.duckdb)
    runner = Path(args.runner)
    base_db = Path(args.base_db)
    template = Path(args.template)
    out_dir = Path(args.out_dir)
    if not base_db.exists():
        raise FileNotFoundError(base_db)
    if args.run_sequential and not runner.exists():
        raise FileNotFoundError(runner)

    remap_percents = parse_int_list(args.remap_percents)
    whales_per_sf_values = parse_int_list(args.whales_per_sf)
    template_config, by_query = load_template_entries(template)
    if args.run_sequential:
        run_sequential(args, duckdb, runner, base_db, template_config, by_query, remap_percents, whales_per_sf_values)
        return 0

    rows = []
    baseline = {
        "variant": "baseline",
        "db": relpath(base_db),
        "sf": args.sf,
        "remap_percent": 0,
        "whales_per_sf": "",
        "n_whales": "",
    }
    baseline.update(collect_db_stats(duckdb, base_db))
    rows.append(baseline)

    skew_variants = []
    for remap_percent in remap_percents:
        for whales_per_sf in whales_per_sf_values:
            n_whales = whales_per_sf * args.sf
            variant = f"r{remap_percent}_w{whales_per_sf}"
            db_path = out_dir / f"controlled_skew_sf{args.sf}_{variant}.db"
            skew_db(duckdb, base_db, db_path, remap_percent, n_whales, args.generate_threads, args.force)
            row = {
                "variant": variant,
                "db": relpath(db_path),
                "sf": args.sf,
                "remap_percent": remap_percent,
                "whales_per_sf": whales_per_sf,
                "n_whales": n_whales,
            }
            row.update(collect_db_stats(duckdb, db_path))
            rows.append(row)
            skew_variants.append(row)

    metadata_path = out_dir / f"controlled_skew_sf{args.sf}_metadata.csv"
    write_metadata(metadata_path, rows)

    full_config = out_dir / f"controlled_skew_sf{args.sf}_allmodes_{args.runs}run.json"
    full_results = out_dir / f"controlled_skew_sf{args.sf}_allmodes_{args.runs}run_results.csv"
    make_config(template_config, by_query, rows, full_config, full_results, args.runs, args.threads, QUERY_ORDER)

    current_variant = next((row for row in rows if row["variant"] == "r30_w150"), skew_variants[0] if skew_variants else rows[0])
    smoke_config = out_dir / f"controlled_skew_sf{args.sf}_smoke_{args.smoke_query}.json"
    smoke_results = out_dir / f"controlled_skew_sf{args.sf}_smoke_{args.smoke_query}_results.csv"
    make_config(template_config, by_query, [current_variant], smoke_config, smoke_results, args.smoke_runs, args.threads,
                [args.smoke_query])

    print(f"[skew] wrote {metadata_path}")
    print(f"[skew] wrote {full_config}")
    print(f"[skew] wrote {smoke_config}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
