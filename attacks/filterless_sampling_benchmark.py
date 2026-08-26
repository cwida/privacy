#!/usr/bin/env python3
"""Evaluate Peter's sampled-inactive execution shortcut for filterless group metadata.

All PUs with at least one qualifying row in a group are retained. A deterministic hash sample of
the remaining PUs is retained at rate 1/q. Two HLL estimates are kept per group and combined as

    HLL(active PUs) + q * HLL(sampled inactive PUs).

The sketches are deliberately separate: duplicating an identifier inside one HLL does not multiply
its cardinality. This script measures execution time and support-estimation error only. HLL and
sampling are not DP mechanisms and their output must not be used as an unnoised release gate.
"""

import argparse
import time

import numpy as np


GROUPINGS = {
	"month": "strftime(l_shipdate,'%Y-%m')",
	"month|nation": "strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar)",
}

PREDICATES = {
	"customer balance": "c_acctbal>=8000",
	"small quantity": "l_quantity<10",
}

FROM = """FROM tpch.lineitem
JOIN tpch.orders ON o_orderkey=l_orderkey
JOIN tpch.customer ON c_custkey=o_custkey"""


def full_sql(group_expr, predicate):
	return f"""WITH pg AS (
		SELECT o_custkey pu, {group_expr} g, bool_or({predicate}) active,
		       sum(CASE WHEN {predicate} THEN l_extendedprice ELSE 0 END) t
		{FROM}
		GROUP BY 1,2)
	SELECT g, count(*) total_pus, count(*) FILTER (WHERE active) active_pus,
	       approx_count_distinct(pu) total_hll,
	       coalesce(approx_count_distinct(pu) FILTER (WHERE active),0) active_hll,
	       sum(t) query_value
	FROM pg GROUP BY 1 ORDER BY 1"""


def sampled_sql(group_expr, predicate, q, salt):
	return f"""WITH pg AS (
		SELECT o_custkey pu, {group_expr} g, bool_or({predicate}) active,
		       sum(CASE WHEN {predicate} THEN l_extendedprice ELSE 0 END) t
		{FROM}
		WHERE ({predicate}) OR hash(cast(o_custkey AS bigint)+{salt * 1000003})%{q}=0
		GROUP BY 1,2)
	SELECT g,
	       count(*) FILTER (WHERE active) active_exact,
	       count(*) FILTER (WHERE NOT active) inactive_sample_exact,
	       coalesce(approx_count_distinct(pu) FILTER (WHERE active),0) active_hll,
	       coalesce(approx_count_distinct(pu) FILTER (WHERE NOT active),0) inactive_sample_hll,
	       sum(t) query_value
	FROM pg GROUP BY 1 ORDER BY 1"""


def filtered_sql(group_expr, predicate):
	return f"""WITH pg AS (
		SELECT o_custkey pu, {group_expr} g, sum(l_extendedprice) t
		{FROM}
		WHERE {predicate}
		GROUP BY 1,2)
	SELECT g, count(*) active_pus, sum(t) query_value FROM pg GROUP BY 1 ORDER BY 1"""


def timed(con, sql, repeats):
	con.execute(sql).fetchall()
	times = []
	for _ in range(repeats):
		start = time.perf_counter()
		con.execute(sql).fetchall()
		times.append(time.perf_counter() - start)
	return float(np.median(times))


def rows_by_group(rows):
	return {str(row[0]): row[1:] for row in rows}


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--db", default="/tmp/privacy_filterless_tpch_sf1_v2.db")
	parser.add_argument("--sample-degree", type=int, default=64)
	parser.add_argument("--salts", type=int, default=8)
	parser.add_argument("--repeats", type=int, default=3)
	args = parser.parse_args()
	if args.sample_degree < 2 or args.salts < 1 or args.repeats < 1:
		raise ValueError("sample-degree must be >=2; salts and repeats must be positive")

	import duckdb
	con = duckdb.connect(config={"threads": 2})
	con.execute("SET enable_progress_bar=false")
	con.execute(f"ATTACH '{args.db}' AS tpch (READ_ONLY)")

	print(f"sample nonqualifying PU/groups at 1/{args.sample_degree}; "
	      f"{args.salts} hash salts, median of {args.repeats} timed executions")
	hdr = (f"{'grouping / predicate':<36}{'full s':>9}{'sample s':>10}{'speedup':>9}"
	       f"{'filtered s':>12}{'HT MdAE':>10}{'HT p95':>9}{'HLL MdAE':>11}{'HLL p95':>10}"
	       f"{'full HLL p95':>14}"
	       f"{'value max err':>15}")
	print(hdr)
	print("-" * len(hdr))

	for grouping, group_expr in GROUPINGS.items():
		for pred_name, predicate in PREDICATES.items():
			full_query = full_sql(group_expr, predicate)
			sample_query = sampled_sql(group_expr, predicate, args.sample_degree, 0)
			filter_query = filtered_sql(group_expr, predicate)
			full_time = timed(con, full_query, args.repeats)
			sample_time = timed(con, sample_query, args.repeats)
			filter_time = timed(con, filter_query, args.repeats)

			truth = rows_by_group(con.execute(full_query).fetchall())
			ht_errors = []
			hll_errors = []
			full_hll_errors = []
			value_errors = []
			for salt in range(args.salts):
				est = rows_by_group(con.execute(sampled_sql(
					group_expr, predicate, args.sample_degree, salt)).fetchall())
				for group, (total, _active, total_hll, _active_hll, query_value) in truth.items():
					(active_exact, inactive_exact, active_hll, inactive_hll,
					 sampled_value) = est.get(group, (0, 0, 0, 0, 0.0))
					ht_total = active_exact + args.sample_degree * inactive_exact
					hll_total = active_hll + args.sample_degree * inactive_hll
					ht_errors.append(abs(ht_total - total) / max(total, 1))
					hll_errors.append(abs(hll_total - total) / max(total, 1))
					full_hll_errors.append(abs(total_hll - total) / max(total, 1))
					value_errors.append(abs(float(sampled_value or 0.0) - float(query_value or 0.0)) /
					                    max(abs(float(query_value or 0.0)), 1.0))

			print(f"{grouping+' / '+pred_name:<36}{full_time:>9.3f}{sample_time:>10.3f}"
			      f"{full_time/sample_time:>8.2f}x{filter_time:>12.3f}"
			      f"{100*np.median(ht_errors):>9.2f}%{100*np.quantile(ht_errors,.95):>8.2f}%"
			      f"{100*np.median(hll_errors):>10.2f}%{100*np.quantile(hll_errors,.95):>9.2f}%"
			      f"{100*np.quantile(full_hll_errors,.95):>13.2f}%"
			      f"{100*max(value_errors):>14.6f}%", flush=True)


if __name__ == "__main__":
	main()
