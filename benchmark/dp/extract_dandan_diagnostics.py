#!/usr/bin/env python3
import argparse
import csv
import json
import math
import subprocess
import tempfile
from pathlib import Path


EPSILON = 1.0
DELTA = 2.2222222222222222e-7
SAMPLE_LANES = 1


def sql_quote(text):
	return "'" + text.replace("'", "''") + "'"


def clip_expr(expr, lower, upper):
	return f"least(greatest(CAST({expr} AS DOUBLE), {lower}), {upper})"


def avg_bound_expr(value_expr, count_expr, lower, upper, count_bound):
	return f"as_sample_m_avg(pu_hash, {clip_expr(value_expr, lower, upper)}, {clip_expr(count_expr, 0.0, count_bound)})"


QUERIES = {
	"q01": {
		"key_cols": ["l_returnflag", "l_linestatus"],
		"key_expr": "'{\"l_returnflag\":\"' || l_returnflag || '\",\"l_linestatus\":\"' || l_linestatus || '\"}'",
		"exact_sql": """
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
		"sass": {
			"k": 8,
			"c_u": 5,
			"sum_bound": 1_000_000.0,
			"count_bound": 1000.0,
			"sum_output_bound": 10_000_000_000_000.0,
			"count_output_bound": 100_000_000.0,
			"avg_bounds": {
				"avg_qty": (0.0, 50.0),
				"avg_price": (0.0, 105000.0),
				"avg_disc": (0.0, 0.1),
			},
		},
		"dp": {
			"k": 8,
			"c_u": 5,
			"sum_bound": 1_000_000.0,
			"count_bound": 1000.0,
			"avg_bounds": {
				"avg_qty": (0.0, 50.0),
				"avg_price": (0.0, 105000.0),
				"avg_disc": (0.0, 0.1),
			},
		},
	},
	"q05": {
		"key_cols": ["n_name"],
		"key_expr": "'{\"n_name\":\"' || n_name || '\"}'",
		"exact_sql": """
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
		"sass": {"k": 1, "c_u": 5, "sum_bound": 1_000_000.0, "sum_output_bound": 10_000_000_000.0},
		"dp": {"k": 1, "c_u": 5, "sum_bound": 1_000_000.0},
	},
	"q06": {
		"key_cols": [],
		"key_expr": "'{}'",
		"exact_sql": """
SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= CAST('1994-01-01' AS date)
  AND l_shipdate < CAST('1995-01-01' AS date)
  AND l_discount BETWEEN 0.05 AND 0.07
  AND l_quantity < 24
""",
		"sass": {"k": 1, "c_u": 1, "sum_bound": 100_000.0, "sum_output_bound": 4_000_000_000.0},
		"dp": {"k": 1, "c_u": 1, "sum_bound": 100_000.0},
	},
	"q14": {
		"key_cols": [],
		"key_expr": "'{}'",
		"exact_sql": """
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
		"sass": {"k": 2, "c_u": 1, "sum_bound": 1_000_000.0, "sum_output_bound": 100_000_000_000.0},
		"dp": {"k": 2, "c_u": 1, "sum_bound": 1_000_000.0},
	},
	"q19": {
		"key_cols": [],
		"key_expr": "'{}'",
		"exact_sql": """
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
		"sass": {"k": 1, "c_u": 1, "sum_bound": 100_000.0, "sum_output_bound": 100_000_000.0},
		"dp": {"k": 1, "c_u": 1, "sum_bound": 100_000.0},
	},
}


def q01_lane_sql():
	cfg = QUERIES["q01"]["sass"]
	sb = cfg["sum_bound"]
	cb = cfg["count_bound"]
	ob = cfg["sum_output_bound"]
	cob = cfg["count_output_bound"]
	avg_bounds = cfg["avg_bounds"]
	parts = [
		("sum_qty", f"CAST(sum_qty_lanes AS VARCHAR)", -ob, ob, 0.0),
		("sum_base_price", f"CAST(sum_base_price_lanes AS VARCHAR)", -ob, ob, 0.0),
		("sum_disc_price", f"CAST(sum_disc_price_lanes AS VARCHAR)", -ob, ob, 0.0),
		("sum_charge", f"CAST(sum_charge_lanes AS VARCHAR)", -ob, ob, 0.0),
		("avg_qty", f"CAST(avg_qty_lanes AS VARCHAR)", avg_bounds["avg_qty"][0], avg_bounds["avg_qty"][1], 25.0),
		("avg_price", f"CAST(avg_price_lanes AS VARCHAR)", avg_bounds["avg_price"][0], avg_bounds["avg_price"][1], 52500.0),
		("avg_disc", f"CAST(avg_disc_lanes AS VARCHAR)", avg_bounds["avg_disc"][0], avg_bounds["avg_disc"][1], 0.05),
		("count_order", f"CAST(count_order_lanes AS VARCHAR)", 0.0, cob, 0.0),
	]
	rows = "\nUNION ALL\n".join(
		f"SELECT 'q01' AS query, {QUERIES['q01']['key_expr']} AS group_key_json, '{name}' AS aggregate_name, "
		f"{lo} AS output_lower_bound, {hi} AS output_upper_bound, {default} AS empty_default, {expr} AS lane_outputs_json "
		f"FROM lanes"
		for name, expr, lo, hi, default in parts
	)
	return f"""
WITH per_pu AS (
  SELECT
    l_returnflag,
    l_linestatus,
    o_custkey,
    hash(o_custkey)::UBIGINT AS pu_hash,
    sum(l_quantity)::DOUBLE AS sum_qty,
    sum(l_extendedprice)::DOUBLE AS sum_base_price,
    sum(l_extendedprice * (1 - l_discount))::DOUBLE AS sum_disc_price,
    sum(l_extendedprice * (1 - l_discount) * (1 + l_tax))::DOUBLE AS sum_charge,
    sum(l_quantity)::DOUBLE AS avg_qty_sum,
    count(l_quantity)::DOUBLE AS avg_qty_count,
    sum(l_extendedprice)::DOUBLE AS avg_price_sum,
    count(l_extendedprice)::DOUBLE AS avg_price_count,
    sum(l_discount)::DOUBLE AS avg_disc_sum,
    count(l_discount)::DOUBLE AS avg_disc_count,
    count(*)::DOUBLE AS count_order
  FROM lineitem
  JOIN orders ON l_orderkey = o_orderkey
  WHERE l_shipdate <= CAST('1998-09-02' AS date)
  GROUP BY l_returnflag, l_linestatus, o_custkey
), capped AS (
  SELECT * EXCLUDE rn
  FROM (
    SELECT *, row_number() OVER (
      PARTITION BY pu_hash
      ORDER BY hash(pu_hash, l_returnflag, l_linestatus)
    ) AS rn
    FROM per_pu
  )
  WHERE rn <= 5
), lanes AS (
  SELECT
    l_returnflag,
    l_linestatus,
    as_sample_m_sum(pu_hash, {clip_expr('sum_qty', -sb, sb)}) AS sum_qty_lanes,
    as_sample_m_sum(pu_hash, {clip_expr('sum_base_price', -sb, sb)}) AS sum_base_price_lanes,
    as_sample_m_sum(pu_hash, {clip_expr('sum_disc_price', -sb, sb)}) AS sum_disc_price_lanes,
    as_sample_m_sum(pu_hash, {clip_expr('sum_charge', -sb, sb)}) AS sum_charge_lanes,
    {avg_bound_expr('avg_qty_sum', 'avg_qty_count', -sb, sb, cb)} AS avg_qty_lanes,
    {avg_bound_expr('avg_price_sum', 'avg_price_count', -sb, sb, cb)} AS avg_price_lanes,
    {avg_bound_expr('avg_disc_sum', 'avg_disc_count', -sb, sb, cb)} AS avg_disc_lanes,
    as_sample_m_sum(pu_hash, {clip_expr('count_order', 0.0, cb)}) AS count_order_lanes
  FROM capped
  GROUP BY l_returnflag, l_linestatus
)
{rows}
"""


def q05_lane_sql():
	cfg = QUERIES["q05"]["sass"]
	sb = cfg["sum_bound"]
	ob = cfg["sum_output_bound"]
	return f"""
WITH per_pu AS (
  SELECT
    n_name,
    c_custkey,
    hash(c_custkey)::UBIGINT AS pu_hash,
    sum(l_extendedprice * (1 - l_discount))::DOUBLE AS revenue
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
  GROUP BY n_name, c_custkey
), capped AS (
  SELECT * EXCLUDE rn
  FROM (
    SELECT *, row_number() OVER (
      PARTITION BY pu_hash
      ORDER BY hash(pu_hash, n_name)
    ) AS rn
    FROM per_pu
  )
  WHERE rn <= 5
), lanes AS (
  SELECT
    n_name,
    as_sample_m_sum(pu_hash, {clip_expr('revenue', -sb, sb)}) AS revenue_lanes
  FROM capped
  GROUP BY n_name
)
SELECT 'q05' AS query, {QUERIES['q05']['key_expr']} AS group_key_json, 'revenue' AS aggregate_name,
       {-ob} AS output_lower_bound, {ob} AS output_upper_bound, 0.0 AS empty_default,
       CAST(revenue_lanes AS VARCHAR) AS lane_outputs_json
FROM lanes
"""


def q06_lane_sql():
	cfg = QUERIES["q06"]["sass"]
	sb = cfg["sum_bound"]
	ob = cfg["sum_output_bound"]
	return f"""
WITH per_pu AS (
  SELECT
    o_custkey,
    hash(o_custkey)::UBIGINT AS pu_hash,
    sum(l_extendedprice * l_discount)::DOUBLE AS revenue
  FROM lineitem
  JOIN orders ON l_orderkey = o_orderkey
  WHERE l_shipdate >= CAST('1994-01-01' AS date)
    AND l_shipdate < CAST('1995-01-01' AS date)
    AND l_discount BETWEEN 0.05 AND 0.07
    AND l_quantity < 24
  GROUP BY o_custkey
), lanes AS (
  SELECT as_sample_m_sum(pu_hash, {clip_expr('revenue', -sb, sb)}) AS revenue_lanes
  FROM per_pu
)
SELECT 'q06' AS query, '{{}}' AS group_key_json, 'revenue' AS aggregate_name,
       {-ob} AS output_lower_bound, {ob} AS output_upper_bound, 0.0 AS empty_default,
       CAST(revenue_lanes AS VARCHAR) AS lane_outputs_json
FROM lanes
"""


def q14_lane_sql():
	cfg = QUERIES["q14"]["sass"]
	sb = cfg["sum_bound"]
	ob = cfg["sum_output_bound"]
	return f"""
WITH per_pu AS (
  SELECT
    o_custkey,
    hash(o_custkey)::UBIGINT AS pu_hash,
    sum(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)::DOUBLE AS promo_sum,
    sum(l_extendedprice * (1 - l_discount))::DOUBLE AS total_sum
  FROM lineitem
  JOIN orders ON l_orderkey = o_orderkey
  JOIN part ON l_partkey = p_partkey
  WHERE l_shipdate >= CAST('1995-09-01' AS date)
    AND l_shipdate < CAST('1995-10-01' AS date)
  GROUP BY o_custkey
), lanes AS (
  SELECT
    as_sample_m_sum(pu_hash, {clip_expr('promo_sum', -sb, sb)}) AS promo_sum_lanes,
    as_sample_m_sum(pu_hash, {clip_expr('total_sum', -sb, sb)}) AS total_sum_lanes
  FROM per_pu
), ratio AS (
  SELECT
    promo_sum_lanes,
    total_sum_lanes,
    list_transform(range(1, len(promo_sum_lanes) + 1), lambda i:
      CASE WHEN list_extract(total_sum_lanes, i) IS NULL OR list_extract(total_sum_lanes, i) <= 0
           THEN 0.0
           ELSE least(100.0, greatest(0.0, 100.0 * list_extract(promo_sum_lanes, i) / list_extract(total_sum_lanes, i)))
      END
    ) AS promo_revenue_lanes
  FROM lanes
)
SELECT 'q14' AS query, '{{}}' AS group_key_json, 'promo_sum_component' AS aggregate_name,
       {-ob} AS output_lower_bound, {ob} AS output_upper_bound, 0.0 AS empty_default,
       CAST(promo_sum_lanes AS VARCHAR) AS lane_outputs_json
FROM ratio
UNION ALL
SELECT 'q14' AS query, '{{}}' AS group_key_json, 'total_sum_component' AS aggregate_name,
       {-ob} AS output_lower_bound, {ob} AS output_upper_bound, 0.0 AS empty_default,
       CAST(total_sum_lanes AS VARCHAR) AS lane_outputs_json
FROM ratio
UNION ALL
SELECT 'q14' AS query, '{{}}' AS group_key_json, 'promo_revenue_postprocess' AS aggregate_name,
       0.0 AS output_lower_bound, 100.0 AS output_upper_bound, 0.0 AS empty_default,
       CAST(promo_revenue_lanes AS VARCHAR) AS lane_outputs_json
FROM ratio
"""


def q19_lane_sql():
	cfg = QUERIES["q19"]["sass"]
	sb = cfg["sum_bound"]
	ob = cfg["sum_output_bound"]
	return f"""
WITH per_pu AS (
  SELECT
    o_custkey,
    hash(o_custkey)::UBIGINT AS pu_hash,
    sum(l_extendedprice * (1 - l_discount))::DOUBLE AS revenue
  FROM lineitem
  JOIN orders ON l_orderkey = o_orderkey
  JOIN part ON p_partkey = l_partkey
  WHERE l_shipmode IN ('AIR', 'AIR REG')
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
  GROUP BY o_custkey
), lanes AS (
  SELECT as_sample_m_sum(pu_hash, {clip_expr('revenue', -sb, sb)}) AS revenue_lanes
  FROM per_pu
)
SELECT 'q19' AS query, '{{}}' AS group_key_json, 'revenue' AS aggregate_name,
       {-ob} AS output_lower_bound, {ob} AS output_upper_bound, 0.0 AS empty_default,
       CAST(revenue_lanes AS VARCHAR) AS lane_outputs_json
FROM lanes
"""


LANE_SQL = {
	"q01": q01_lane_sql,
	"q05": q05_lane_sql,
	"q06": q06_lane_sql,
	"q14": q14_lane_sql,
	"q19": q19_lane_sql,
}


def run_duckdb_to_csv(duckdb, db_path, out_path, query, threads, memory_limit, temp_directory, m=None,
                      sass_rescale=True):
	settings = [
		"LOAD privacy",
		f"SET threads={threads}",
		f"SET memory_limit={sql_quote(memory_limit)}",
	]
	if temp_directory:
		settings.append(f"SET temp_directory={sql_quote(str(temp_directory))}")
	if m is not None:
		settings.append(f"SET dp_sample_lanes={SAMPLE_LANES}")
		settings.append(f"SET dp_sass_m={m}")
		settings.append(f"SET dp_sass_rescale={'true' if sass_rescale else 'false'}")
	settings.append(f"COPY ({query}) TO {sql_quote(str(out_path))} (HEADER, DELIMITER ',')")
	sql = ";\n".join(settings) + ";"
	cmd = [str(duckdb), str(db_path), "-c", sql]
	result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
	if result.returncode != 0:
		raise RuntimeError(f"DuckDB failed for {out_path}\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}")


def read_rows(path):
	with open(path, newline="") as f:
		return list(csv.DictReader(f))


def write_rows(path, rows, fieldnames):
	with open(path, "w", newline="") as f:
		writer = csv.DictWriter(f, fieldnames=fieldnames)
		writer.writeheader()
		for row in rows:
			writer.writerow(row)


def lower_median(sorted_values):
	return sorted_values[(len(sorted_values) + 1) // 2 - 1]


def fill_and_clip(raw_values, lower, upper, empty_default):
	fill = min(max(empty_default, lower), upper)
	filled = []
	valid = 0
	for value in raw_values:
		if value is None:
			filled.append(fill)
		else:
			valid += 1
			filled.append(min(max(float(value), lower), upper))
	return filled, valid


def parse_duckdb_numeric_list(text):
	text = text.strip()
	if not (text.startswith("[") and text.endswith("]")):
		raise ValueError(f"expected DuckDB list literal, got: {text[:80]}")
	body = text[1:-1].strip()
	if not body:
		return []
	values = []
	for part in body.split(","):
		item = part.strip()
		if item.upper() == "NULL":
			values.append(None)
		else:
			values.append(float(item))
	return values


def smooth_median_scale(filled_values, epsilon, delta, lower, upper):
	values = sorted(filled_values)
	n = len(values)
	p = (n + 1) // 2
	beta = epsilon / (2.0 * math.log(2.0 / delta))
	x = [lower] + values + [upper]
	smooth = 0.0
	for i in range(0, p + 1):
		for j in range(p, n + 2):
			score = (x[j] - x[i]) * math.exp(-beta * float(j - i - 1))
			if score > smooth:
				smooth = score
	return 2.0 * max(0.0, smooth) / epsilon


def wilson_tau(epsilon_eta, delta_eta, c_u):
	return 1.0 - c_u * math.log(2.0 - 2.0 * math.pow(1.0 - delta_eta, 1.0 / c_u)) / epsilon_eta


def add_saa_metrics(rows, m):
	out = []
	for row in rows:
		query = row["query"]
		cfg = QUERIES[query]["sass"]
		lower = float(row["output_lower_bound"])
		upper = float(row["output_upper_bound"])
		empty_default = float(row["empty_default"])
		raw_values = parse_duckdb_numeric_list(row["lane_outputs_json"])
		filled, valid_count = fill_and_clip(raw_values, lower, upper, empty_default)
		sorted_values = sorted(filled)
		grouped = len(QUERIES[query]["key_cols"]) > 0
		k = float(cfg["k"])
		c_u = float(cfg["c_u"])
		budget_divisor = (k + 1.0 if grouped else k) * c_u
		epsilon_cell = EPSILON / budget_divisor
		delta_cell = DELTA / budget_divisor
		if row["aggregate_name"].endswith("_postprocess"):
			avg_scale = ""
			median_scale = ""
		else:
			avg_scale = (float(SAMPLE_LANES) * (upper - lower)) / (float(m) * epsilon_cell)
			median_scale = smooth_median_scale(filled, epsilon_cell, delta_cell, lower, upper)
		enriched = dict(row)
		enriched.update(
			{
				"m": m,
				"epsilon_cell": epsilon_cell,
				"delta_cell": delta_cell,
				"lane_count": len(raw_values),
				"valid_lane_count": valid_count,
				"lane_outputs_json": json.dumps(raw_values, separators=(",", ":")),
				"filled_clipped_lane_outputs_json": json.dumps(filled, separators=(",", ":")),
				"saa_average_estimator": sum(filled) / len(filled),
				"saa_median_estimator": lower_median(sorted_values),
				"saa_average_noise_scale": avg_scale,
				"saa_median_noise_scale": median_scale,
			}
		)
		out.append(enriched)
	return out


def exact_value_rows(query, exact_rows):
	key_cols = QUERIES[query]["key_cols"]
	rows = []
	for exact in exact_rows:
		if key_cols:
			group_key = json.dumps({col: exact[col] for col in key_cols}, separators=(",", ":"))
		else:
			group_key = "{}"
		for name, value in exact.items():
			if name in key_cols:
				continue
			rows.append({"query": query, "group_key_json": group_key, "aggregate_name": name, "exact_value": value})
	return rows


def dp_bounded_rows(query, exact_rows):
	cfg = QUERIES[query]["dp"]
	k = float(cfg["k"])
	c_u = float(cfg["c_u"])
	grouped = len(QUERIES[query]["key_cols"]) > 0
	budget_units = k + 1.0 if grouped else k
	rows = []
	for exact in exact_value_rows(query, exact_rows):
		name = exact["aggregate_name"]
		base = {
			"query": query,
			"group_key_json": exact["group_key_json"],
			"visible_aggregate": name,
			"exact_value": exact["exact_value"],
			"c_u": c_u,
			"budget_units": budget_units,
			"partition_selection_noise_scale": "" if c_u == 1 else c_u / (EPSILON / (k + 1.0)),
			"partition_selection_tau": "" if c_u == 1 else wilson_tau(EPSILON / (k + 1.0), DELTA / (k + 1.0), c_u),
		}
		if query == "q01" and name.startswith("avg_"):
			lo, hi = cfg["avg_bounds"][name]
			sum_per_pu = ((hi - lo) / 2.0) * cfg["count_bound"]
			count_per_pu = cfg["count_bound"]
			rows.append(
				{
					**base,
					"noisy_cell": f"{name}_bounded_mean_sum_component",
					"contribution_bound_per_pu": sum_per_pu,
					"sensitivity_with_c_u": sum_per_pu * c_u,
					"dp_laplace_noise_scale": sum_per_pu * c_u * budget_units * 2.0 / EPSILON,
				}
			)
			rows.append(
				{
					**base,
					"noisy_cell": f"{name}_count_component",
					"contribution_bound_per_pu": count_per_pu,
					"sensitivity_with_c_u": count_per_pu * c_u,
					"dp_laplace_noise_scale": count_per_pu * c_u * budget_units * 2.0 / EPSILON,
				}
			)
		elif name.startswith("count"):
			per_pu = cfg["count_bound"]
			rows.append(
				{
					**base,
					"noisy_cell": name,
					"contribution_bound_per_pu": per_pu,
					"sensitivity_with_c_u": per_pu * c_u,
					"dp_laplace_noise_scale": per_pu * c_u * budget_units / EPSILON,
				}
			)
		elif query == "q14" and name == "promo_revenue":
			per_pu = cfg["sum_bound"]
			for component in ["promo_sum_component", "total_sum_component"]:
				rows.append(
					{
						**base,
						"noisy_cell": component,
						"contribution_bound_per_pu": per_pu,
						"sensitivity_with_c_u": per_pu * c_u,
						"dp_laplace_noise_scale": per_pu * c_u * budget_units / EPSILON,
					}
				)
		else:
			per_pu = cfg["sum_bound"]
			rows.append(
				{
					**base,
					"noisy_cell": name,
					"contribution_bound_per_pu": per_pu,
					"sensitivity_with_c_u": per_pu * c_u,
					"dp_laplace_noise_scale": per_pu * c_u * budget_units / EPSILON,
				}
			)
	return rows


def first_number(values, default=None):
	if values is None or len(values) == 0:
		return default
	return float(values[0])


def apply_config(config_path, bound_multiplier):
	global EPSILON, DELTA, SAMPLE_LANES
	with open(config_path) as f:
		config = json.load(f)
	EPSILON = first_number(config.get("epsilons"), EPSILON)
	DELTA = first_number(config.get("deltas"), DELTA)
	SAMPLE_LANES = int(first_number(config.get("sample_lanes"), SAMPLE_LANES))
	for dataset in config.get("datasets", []):
		query_names = dataset.get("query_names", [])
		if len(query_names) != 1:
			continue
		query = query_names[0]
		if query not in QUERIES:
			continue
		count_bound = first_number(dataset.get("count_bounds"))
		sum_bound = first_number(dataset.get("sum_bounds"))
		c_u = first_number(dataset.get("c_u"), first_number(dataset.get("group_bounds"), 1.0))
		sass_count_output_bound = first_number(dataset.get("sass_count_output_bounds"))
		sass_sum_output_bound = first_number(dataset.get("sass_sum_output_bounds"))
		for mode_key in ["sass", "dp"]:
			target = QUERIES[query][mode_key]
			if count_bound is not None:
				target["count_bound"] = count_bound * bound_multiplier
			if sum_bound is not None:
				target["sum_bound"] = sum_bound * bound_multiplier
			if c_u is not None:
				target["c_u"] = c_u
		if sass_count_output_bound is not None:
			QUERIES[query]["sass"]["count_output_bound"] = sass_count_output_bound
		if sass_sum_output_bound is not None:
			QUERIES[query]["sass"]["sum_output_bound"] = sass_sum_output_bound
		avg_lowers = dataset.get("avg_lower_bounds", [])
		avg_uppers = dataset.get("avg_upper_bounds", [])
		if query == "q01" and avg_lowers and avg_uppers:
			avg_names = ["avg_qty", "avg_price", "avg_disc"]
			avg_bounds = {name: (float(avg_lowers[i]), float(avg_uppers[i])) for i, name in enumerate(avg_names)}
			QUERIES[query]["sass"]["avg_bounds"] = avg_bounds
			QUERIES[query]["dp"]["avg_bounds"] = avg_bounds


def main():
	parser = argparse.ArgumentParser(description="Extract unskewed SF30 DP/SAA diagnostics for Dandan.")
	parser.add_argument("--duckdb", default="build/release/duckdb")
	parser.add_argument("--db", default="tpch_sf30_graviton.db")
	parser.add_argument("--out-dir", default="/mnt/clickbench_tmp/dp_dandan_diagnostics")
	parser.add_argument("--config", default="")
	parser.add_argument("--bound-multiplier", type=float, default=1.0)
	parser.add_argument("--threads", type=int, default=16)
	parser.add_argument("--memory-limit", default="16GB")
	parser.add_argument("--temp-directory", default="/mnt/clickbench_tmp")
	parser.add_argument("--ms", default="64,512")
	parser.add_argument("--sass-rescale", action=argparse.BooleanOptionalAction, default=True)
	args = parser.parse_args()

	if args.config:
		apply_config(args.config, args.bound_multiplier)

	duckdb = Path(args.duckdb)
	db_path = Path(args.db)
	out_dir = Path(args.out_dir)
	out_dir.mkdir(parents=True, exist_ok=True)
	temp_directory = Path(args.temp_directory) if args.temp_directory else None
	ms = [int(part.strip()) for part in args.ms.split(",") if part.strip()]

	exact_all = []
	dp_rows = []
	lane_all = []

	with tempfile.TemporaryDirectory(dir=str(out_dir)) as tmp:
		tmp_dir = Path(tmp)
		for query, cfg in QUERIES.items():
			print(f"[diagnostics] exact {query}", flush=True)
			exact_path = out_dir / f"exact_{query}.csv"
			run_duckdb_to_csv(
				duckdb,
				db_path,
				exact_path,
				cfg["exact_sql"],
				args.threads,
				args.memory_limit,
				temp_directory,
			)
			exact_rows = read_rows(exact_path)
			exact_all.extend(exact_value_rows(query, exact_rows))
			dp_rows.extend(dp_bounded_rows(query, exact_rows))
			for m in ms:
				print(f"[diagnostics] lanes {query} m={m}", flush=True)
				lane_tmp = tmp_dir / f"lanes_{query}_m{m}.csv"
				run_duckdb_to_csv(
					duckdb,
					db_path,
					lane_tmp,
					LANE_SQL[query](),
					args.threads,
					args.memory_limit,
					temp_directory,
					m=m,
					sass_rescale=args.sass_rescale,
				)
				lane_all.extend(add_saa_metrics(read_rows(lane_tmp), m))

	write_rows(
		out_dir / "unskewed_sf30_exact_visible_answers.csv",
		exact_all,
		["query", "group_key_json", "aggregate_name", "exact_value"],
	)
	write_rows(
		out_dir / "unskewed_sf30_dp_bounded_diagnostics.csv",
		dp_rows,
		[
			"query",
			"group_key_json",
			"visible_aggregate",
			"exact_value",
			"noisy_cell",
			"contribution_bound_per_pu",
			"c_u",
			"sensitivity_with_c_u",
			"budget_units",
			"dp_laplace_noise_scale",
			"partition_selection_noise_scale",
			"partition_selection_tau",
		],
	)
	lane_fields = [
		"query",
		"m",
		"group_key_json",
		"aggregate_name",
		"output_lower_bound",
		"output_upper_bound",
		"empty_default",
		"epsilon_cell",
		"delta_cell",
		"lane_count",
		"valid_lane_count",
		"saa_average_estimator",
		"saa_median_estimator",
		"saa_average_noise_scale",
		"saa_median_noise_scale",
		"lane_outputs_json",
		"filled_clipped_lane_outputs_json",
	]
	write_rows(out_dir / "unskewed_sf30_saa_lane_diagnostics.csv", lane_all, lane_fields)
	compact = [{k: v for k, v in row.items() if k not in {"lane_outputs_json", "filled_clipped_lane_outputs_json"}} for row in lane_all]
	write_rows(out_dir / "unskewed_sf30_saa_summary.csv", compact, [f for f in lane_fields if f not in {"lane_outputs_json", "filled_clipped_lane_outputs_json"}])
	print(f"[diagnostics] wrote {out_dir}", flush=True)


if __name__ == "__main__":
	main()
