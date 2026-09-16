#!/usr/bin/env python3
"""Benchmark filterless aggregate-component overhead on a TPC-H database.

The workload deliberately varies the number of aggregate components while
keeping the scan, filter, privacy-unit chain, and grouping keys fixed.
"""

import argparse
import re
import statistics
import subprocess
from pathlib import Path


QUERIES = {
	"ungrouped_sum_1": """
		SELECT SUM(l_extendedprice::DOUBLE)
		FROM lineitem
		WHERE l_quantity < 10
	""",
	"ungrouped_sum_count_2": """
		SELECT SUM(l_extendedprice::DOUBLE), COUNT(*)
		FROM lineitem
		WHERE l_quantity < 10
	""",
	"ungrouped_mixed_5": """
		SELECT SUM(l_quantity::DOUBLE),
		       SUM(l_extendedprice::DOUBLE),
		       SUM((l_extendedprice * (1 - l_discount))::DOUBLE),
		       SUM((l_extendedprice * (1 - l_discount) * (1 + l_tax))::DOUBLE),
		       COUNT(*)
		FROM lineitem
		WHERE l_quantity < 10
	""",
	"ungrouped_avg_3": """
		SELECT AVG(l_quantity::DOUBLE),
		       AVG(l_extendedprice::DOUBLE),
		       AVG(l_discount::DOUBLE)
		FROM lineitem
		WHERE l_quantity < 10
	""",
	"grouped_sum_1": """
		SELECT l_returnflag, l_linestatus, SUM(l_extendedprice::DOUBLE)
		FROM lineitem
		WHERE l_quantity < 10
		GROUP BY l_returnflag, l_linestatus
	""",
	"grouped_mixed_5": """
		SELECT l_returnflag, l_linestatus,
		       SUM(l_quantity::DOUBLE),
		       SUM(l_extendedprice::DOUBLE),
		       SUM((l_extendedprice * (1 - l_discount))::DOUBLE),
		       SUM((l_extendedprice * (1 - l_discount) * (1 + l_tax))::DOUBLE),
		       COUNT(*)
		FROM lineitem
		WHERE l_quantity < 10
		GROUP BY l_returnflag, l_linestatus
	""",
	"grouped_avg_3": """
		SELECT l_returnflag, l_linestatus,
		       AVG(l_quantity::DOUBLE),
		       AVG(l_extendedprice::DOUBLE),
		       AVG(l_discount::DOUBLE)
		FROM lineitem
		WHERE l_quantity < 10
		GROUP BY l_returnflag, l_linestatus
	""",
}


TIME_PATTERN = re.compile(r"Run Time \(s\): real ([0-9.]+)")


def sql_literal(value: Path) -> str:
	return "'" + str(value).replace("'", "''") + "'"


def benchmark_query(args, name, query):
	setup = f"""
.mode trash
.timer off
LOAD {sql_literal(args.extension)};
SET threads={args.threads};
SET privacy_mode='dp_filterless';
SET dp_epsilon=1.0;
SET dp_delta=0.000001;
SET dp_filterless_sample_bits=6;
SET dp_filterless_clip_support=128.0;
SET dp_filterless_noise_bounds=true;
SET dp_filterless_bounds_epsilon_fraction=0.25;
SET dp_max_groups_contributed=4;
SET privacy_noise=true;
{query};
.timer on
"""
	script = setup + "\n".join(f"{query};" for _ in range(args.runs)) + "\n"
	process = subprocess.run(
		[str(args.duckdb), str(args.database), "-unsigned"],
		input=script,
		text=True,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
		check=False,
	)
	if process.returncode != 0:
		raise RuntimeError(f"{name} failed:\n{process.stdout}")
	times = [float(value) for value in TIME_PATTERN.findall(process.stdout)]
	if len(times) != args.runs:
		raise RuntimeError(f"{name}: expected {args.runs} timings, found {len(times)}:\n{process.stdout}")
	return times


def main():
	root = Path(__file__).resolve().parents[2]
	parser = argparse.ArgumentParser()
	parser.add_argument("--database", type=Path, default=Path("/private/tmp/tpch_sf30_filterless.db"))
	parser.add_argument("--duckdb", type=Path, default=root / "build/release/duckdb")
	parser.add_argument(
		"--extension",
		type=Path,
		default=root / "build/release/extension/privacy/privacy.duckdb_extension",
	)
	parser.add_argument("--runs", type=int, default=5)
	parser.add_argument("--threads", type=int, default=4)
	parser.add_argument("--query", action="append", choices=QUERIES, help="run only the named query (repeatable)")
	args = parser.parse_args()
	if args.runs < 1 or args.threads < 1:
		parser.error("--runs and --threads must be positive")

	selected_queries = args.query or QUERIES.keys()
	print(f"SF30 filterless fusion benchmark: {args.runs} timed runs after one warm-up")
	print(f"{'query':<26} {'median s':>10} {'min s':>10} {'max s':>10}")
	print("-" * 59)
	for name in selected_queries:
		query = QUERIES[name]
		times = benchmark_query(args, name, query.strip())
		print(f"{name:<26} {statistics.median(times):>10.3f} {min(times):>10.3f} {max(times):>10.3f}", flush=True)


if __name__ == "__main__":
	main()
