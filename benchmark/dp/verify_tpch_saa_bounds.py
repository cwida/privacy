#!/usr/bin/env python3
import argparse
import csv
import json
import math
import subprocess
from collections import defaultdict
from pathlib import Path


SUM_AGGREGATES = {
	"q01": ["sum_qty", "sum_base_price", "sum_disc_price", "sum_charge"],
	"q05": ["revenue"],
	"q06": ["revenue"],
	"q14": ["promo_sum_component", "total_sum_component"],
	"q19": ["revenue"],
}

AVG_AGGREGATES = {
	"q01": ["avg_qty", "avg_price", "avg_disc"],
}

COUNT_AGGREGATES = {
	"q01": ["count_order"],
}


def sql_quote(value):
	return "'" + str(value).replace("'", "''") + "'"


def clip_expr(expr, lower, upper):
	return f"least(greatest(CAST({expr} AS DOUBLE), {lower}), {upper})"


def parse_number_list(value):
	if value is None:
		return []
	if isinstance(value, list):
		if not value:
			return []
		if len(value) == 1 and isinstance(value[0], str):
			value = value[0]
		else:
			return [float(part) for part in value]
	if isinstance(value, (int, float)):
		return [float(value)]
	text = str(value).strip()
	if not text:
		return []
	return [float(part.strip()) for part in text.split(",") if part.strip()]


def first_number(value, default=None):
	values = parse_number_list(value)
	return values[0] if values else default


def quantile_cont(values, q):
	clean = sorted(v for v in values if v is not None and math.isfinite(v))
	if not clean:
		return None
	if len(clean) == 1:
		return clean[0]
	pos = (len(clean) - 1) * q
	lo = math.floor(pos)
	hi = math.ceil(pos)
	if lo == hi:
		return clean[lo]
	frac = pos - lo
	return clean[lo] + (clean[hi] - clean[lo]) * frac


def fmt_number(value):
	if value is None:
		return ""
	if isinstance(value, str):
		return value
	if math.isfinite(value) and abs(value - round(value)) < 1e-9:
		return str(int(round(value)))
	return f"{value:.17g}"


def list_setting(values):
	return ",".join(fmt_number(v) for v in values)


def config_sass_points(config):
	points = {}
	for dataset in config.get("datasets", []):
		if dataset.get("modes") != ["dp_sass"]:
			continue
		query_names = dataset.get("query_names", [])
		if len(query_names) != 1:
			continue
		query = query_names[0]
		for m in parse_number_list(dataset.get("sass_ms")):
			points[(query, int(m))] = dataset
	return points


def q01_lane_sql(m, dataset):
	sum_bounds = parse_number_list(dataset.get("sum_bound_lists"))
	if len(sum_bounds) != 4:
		sum_bounds = [first_number(dataset.get("sum_bounds"), 0.0)] * 4
	count_bound = first_number(dataset.get("count_bounds"), 0.0)
	group_bound = int(first_number(dataset.get("c_u"), 5))
	return f"""
WITH per_pu AS (
	SELECT
		l.l_returnflag,
		l.l_linestatus,
		hash(o.o_custkey)::UBIGINT AS pu_hash,
		SUM(l.l_quantity)::DOUBLE AS sum_qty,
		SUM(l.l_extendedprice)::DOUBLE AS sum_base_price,
		SUM(l.l_extendedprice * (1 - l.l_discount))::DOUBLE AS sum_disc_price,
		SUM(l.l_extendedprice * (1 - l.l_discount) * (1 + l.l_tax))::DOUBLE AS sum_charge,
		SUM(l.l_quantity)::DOUBLE AS avg_qty_sum,
		COUNT(l.l_quantity)::DOUBLE AS avg_qty_count,
		SUM(l.l_extendedprice)::DOUBLE AS avg_price_sum,
		COUNT(l.l_extendedprice)::DOUBLE AS avg_price_count,
		SUM(l.l_discount)::DOUBLE AS avg_disc_sum,
		COUNT(l.l_discount)::DOUBLE AS avg_disc_count,
		COUNT(*)::DOUBLE AS count_order
	FROM lineitem l
	JOIN orders o ON l.l_orderkey = o.o_orderkey
	WHERE l.l_shipdate <= CAST('1998-09-02' AS DATE)
	GROUP BY l.l_returnflag, l.l_linestatus, hash(o.o_custkey)::UBIGINT
), capped AS (
	SELECT * EXCLUDE rn
	FROM (
		SELECT *, row_number() OVER (
			PARTITION BY pu_hash
			ORDER BY hash(pu_hash, l_returnflag, l_linestatus)
		) AS rn
		FROM per_pu
	)
	WHERE rn <= {group_bound}
), lanes AS (
	SELECT
		l_returnflag,
		l_linestatus,
		as_sample_m_sum(pu_hash, {clip_expr("sum_qty", -sum_bounds[0], sum_bounds[0])}) AS sum_qty,
		as_sample_m_sum(pu_hash, {clip_expr("sum_base_price", -sum_bounds[1], sum_bounds[1])}) AS sum_base_price,
		as_sample_m_sum(pu_hash, {clip_expr("sum_disc_price", -sum_bounds[2], sum_bounds[2])}) AS sum_disc_price,
		as_sample_m_sum(pu_hash, {clip_expr("sum_charge", -sum_bounds[3], sum_bounds[3])}) AS sum_charge,
		as_sample_m_avg(pu_hash, {clip_expr("avg_qty_sum", -first_number(dataset.get("sum_bounds"), 0.0), first_number(dataset.get("sum_bounds"), 0.0))}, {clip_expr("avg_qty_count", 0.0, count_bound)}) AS avg_qty,
		as_sample_m_avg(pu_hash, {clip_expr("avg_price_sum", -first_number(dataset.get("sum_bounds"), 0.0), first_number(dataset.get("sum_bounds"), 0.0))}, {clip_expr("avg_price_count", 0.0, count_bound)}) AS avg_price,
		as_sample_m_avg(pu_hash, {clip_expr("avg_disc_sum", -first_number(dataset.get("sum_bounds"), 0.0), first_number(dataset.get("sum_bounds"), 0.0))}, {clip_expr("avg_disc_count", 0.0, count_bound)}) AS avg_disc,
		as_sample_m_sum(pu_hash, {clip_expr("count_order", 0.0, count_bound)}) AS count_order
	FROM capped
	GROUP BY l_returnflag, l_linestatus
), unnested AS (
	SELECT 'sum_qty' AS aggregate_name, l_returnflag, l_linestatus, unnest(sum_qty) AS lane_value, generate_subscripts(sum_qty, 1) AS lane FROM lanes
	UNION ALL SELECT 'sum_base_price', l_returnflag, l_linestatus, unnest(sum_base_price), generate_subscripts(sum_base_price, 1) FROM lanes
	UNION ALL SELECT 'sum_disc_price', l_returnflag, l_linestatus, unnest(sum_disc_price), generate_subscripts(sum_disc_price, 1) FROM lanes
	UNION ALL SELECT 'sum_charge', l_returnflag, l_linestatus, unnest(sum_charge), generate_subscripts(sum_charge, 1) FROM lanes
	UNION ALL SELECT 'avg_qty', l_returnflag, l_linestatus, unnest(avg_qty), generate_subscripts(avg_qty, 1) FROM lanes
	UNION ALL SELECT 'avg_price', l_returnflag, l_linestatus, unnest(avg_price), generate_subscripts(avg_price, 1) FROM lanes
	UNION ALL SELECT 'avg_disc', l_returnflag, l_linestatus, unnest(avg_disc), generate_subscripts(avg_disc, 1) FROM lanes
	UNION ALL SELECT 'count_order', l_returnflag, l_linestatus, unnest(count_order), generate_subscripts(count_order, 1) FROM lanes
)
SELECT 'q01' AS query, {m} AS m,
       '{{"l_returnflag":"'||l_returnflag||'","l_linestatus":"'||l_linestatus||'"}}' AS group_key,
       aggregate_name, lane::INTEGER AS lane, lane_value::DOUBLE AS lane_value
FROM unnested
"""


def q05_lane_sql(m, dataset):
	sum_bound = first_number(dataset.get("sum_bounds"), 0.0)
	group_bound = int(first_number(dataset.get("c_u"), 5))
	return f"""
WITH per_pu AS (
	SELECT
		n_name,
		hash(c_custkey)::UBIGINT AS pu_hash,
		SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS revenue
	FROM customer, orders, lineitem, supplier, nation, region
	WHERE c_custkey = o_custkey
	  AND l_orderkey = o_orderkey
	  AND l_suppkey = s_suppkey
	  AND c_nationkey = s_nationkey
	  AND s_nationkey = n_nationkey
	  AND n_regionkey = r_regionkey
	  AND r_name = 'ASIA'
	  AND o_orderdate >= CAST('1994-01-01' AS DATE)
	  AND o_orderdate < CAST('1995-01-01' AS DATE)
	GROUP BY n_name, hash(c_custkey)::UBIGINT
), capped AS (
	SELECT * EXCLUDE rn
	FROM (
		SELECT *, row_number() OVER (
			PARTITION BY pu_hash
			ORDER BY hash(pu_hash, n_name)
		) AS rn
		FROM per_pu
	)
	WHERE rn <= {group_bound}
), lanes AS (
	SELECT n_name, as_sample_m_sum(pu_hash, {clip_expr("revenue", -sum_bound, sum_bound)}) AS revenue
	FROM capped
	GROUP BY n_name
), unnested AS (
	SELECT n_name, unnest(revenue) AS lane_value, generate_subscripts(revenue, 1) AS lane FROM lanes
)
SELECT 'q05' AS query, {m} AS m, '{{"n_name":"'||n_name||'"}}' AS group_key,
       'revenue' AS aggregate_name, lane::INTEGER AS lane, lane_value::DOUBLE AS lane_value
FROM unnested
"""


def q06_lane_sql(m, dataset):
	sum_bound = first_number(dataset.get("sum_bounds"), 0.0)
	return f"""
WITH per_pu AS (
	SELECT
		hash(o_custkey)::UBIGINT AS pu_hash,
		SUM(l_extendedprice * l_discount)::DOUBLE AS revenue
	FROM lineitem
	JOIN orders ON l_orderkey = o_orderkey
	WHERE l_shipdate >= CAST('1994-01-01' AS DATE)
	  AND l_shipdate < CAST('1995-01-01' AS DATE)
	  AND l_discount BETWEEN 0.05 AND 0.07
	  AND l_quantity < 24
	GROUP BY hash(o_custkey)::UBIGINT
), lanes AS (
	SELECT as_sample_m_sum(pu_hash, {clip_expr("revenue", -sum_bound, sum_bound)}) AS revenue
	FROM per_pu
), unnested AS (
	SELECT unnest(revenue) AS lane_value, generate_subscripts(revenue, 1) AS lane FROM lanes
)
SELECT 'q06' AS query, {m} AS m, '{{}}' AS group_key,
       'revenue' AS aggregate_name, lane::INTEGER AS lane, lane_value::DOUBLE AS lane_value
FROM unnested
"""


def q14_lane_sql(m, dataset):
	sum_bounds = parse_number_list(dataset.get("sum_bound_lists"))
	if len(sum_bounds) != 2:
		sum_bounds = [first_number(dataset.get("sum_bounds"), 0.0)] * 2
	return f"""
WITH per_pu AS (
	SELECT
		hash(o_custkey)::UBIGINT AS pu_hash,
		SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)::DOUBLE AS promo_sum,
		SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS total_sum
	FROM lineitem
	JOIN orders ON l_orderkey = o_orderkey
	JOIN part ON l_partkey = p_partkey
	WHERE l_shipdate >= CAST('1995-09-01' AS DATE)
	  AND l_shipdate < CAST('1995-10-01' AS DATE)
	GROUP BY hash(o_custkey)::UBIGINT
), lanes AS (
	SELECT
		as_sample_m_sum(pu_hash, {clip_expr("promo_sum", -sum_bounds[0], sum_bounds[0])}) AS promo_sum_component,
		as_sample_m_sum(pu_hash, {clip_expr("total_sum", -sum_bounds[1], sum_bounds[1])}) AS total_sum_component
	FROM per_pu
), unnested AS (
	SELECT 'promo_sum_component' AS aggregate_name, unnest(promo_sum_component) AS lane_value, generate_subscripts(promo_sum_component, 1) AS lane FROM lanes
	UNION ALL
	SELECT 'total_sum_component', unnest(total_sum_component), generate_subscripts(total_sum_component, 1) FROM lanes
)
SELECT 'q14' AS query, {m} AS m, '{{}}' AS group_key,
       aggregate_name, lane::INTEGER AS lane, lane_value::DOUBLE AS lane_value
FROM unnested
"""


def q19_lane_sql(m, dataset):
	sum_bound = first_number(dataset.get("sum_bounds"), 0.0)
	return f"""
WITH per_pu AS (
	SELECT
		hash(o_custkey)::UBIGINT AS pu_hash,
		SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS revenue
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
	GROUP BY hash(o_custkey)::UBIGINT
), lanes AS (
	SELECT as_sample_m_sum(pu_hash, {clip_expr("revenue", -sum_bound, sum_bound)}) AS revenue
	FROM per_pu
), unnested AS (
	SELECT unnest(revenue) AS lane_value, generate_subscripts(revenue, 1) AS lane FROM lanes
)
SELECT 'q19' AS query, {m} AS m, '{{}}' AS group_key,
       'revenue' AS aggregate_name, lane::INTEGER AS lane, lane_value::DOUBLE AS lane_value
FROM unnested
"""


LANE_SQL = {
	"q01": q01_lane_sql,
	"q05": q05_lane_sql,
	"q06": q06_lane_sql,
	"q14": q14_lane_sql,
	"q19": q19_lane_sql,
}


def contribution_sql(query, dataset):
	if query == "q01":
		sum_bounds = parse_number_list(dataset.get("sum_bound_lists"))
		count_bound = first_number(dataset.get("count_bounds"), 0.0)
		return f"""
WITH q01_base AS (
	SELECT
		l.l_returnflag,
		l.l_linestatus,
		hash(o.o_custkey)::UBIGINT AS pu_hash,
		l.l_quantity::DOUBLE AS l_quantity,
		l.l_extendedprice::DOUBLE AS l_extendedprice,
		l.l_discount::DOUBLE AS l_discount,
		l.l_tax::DOUBLE AS l_tax
	FROM lineitem l
	JOIN orders o ON l.l_orderkey = o.o_orderkey
	WHERE l.l_shipdate <= CAST('1998-09-02' AS DATE)
), per_pu AS (
	SELECT
		l_returnflag,
		l_linestatus,
		pu_hash,
		SUM(l_quantity)::DOUBLE AS sum_qty,
		SUM(l_extendedprice)::DOUBLE AS sum_base_price,
		SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS sum_disc_price,
		SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax))::DOUBLE AS sum_charge,
		COUNT(*)::DOUBLE AS count_order
	FROM q01_base
	GROUP BY l_returnflag, l_linestatus, pu_hash
), values AS (
	SELECT 'sum_qty' AS aggregate_name, sum_qty AS value, {sum_bounds[0]} AS configured_bound FROM per_pu
	UNION ALL SELECT 'sum_base_price', sum_base_price, {sum_bounds[1]} FROM per_pu
	UNION ALL SELECT 'sum_disc_price', sum_disc_price, {sum_bounds[2]} FROM per_pu
	UNION ALL SELECT 'sum_charge', sum_charge, {sum_bounds[3]} FROM per_pu
	UNION ALL SELECT 'count_order', count_order, {count_bound} FROM per_pu
), domains AS (
	SELECT 'avg_qty' AS aggregate_name, l_quantity AS value, 0.0 AS configured_lower, 50.0 AS configured_upper
	FROM q01_base
	UNION ALL
	SELECT 'avg_price', l_extendedprice, 0.0, 105000.0
	FROM q01_base
	UNION ALL
	SELECT 'avg_disc', l_discount, 0.0, 0.1
	FROM q01_base
)
SELECT 'q01' AS query, aggregate_name, 'contribution' AS bound_kind,
       NULL::DOUBLE AS configured_lower, configured_bound AS configured_upper,
       MIN(value)::DOUBLE AS observed_min, MAX(value)::DOUBLE AS observed_max,
       MAX(abs(value))::DOUBLE AS observed_max_abs
FROM values
GROUP BY ALL
UNION ALL
SELECT 'q01' AS query, aggregate_name, 'avg_value_domain' AS bound_kind,
       configured_lower, configured_upper,
       MIN(value)::DOUBLE AS observed_min, MAX(value)::DOUBLE AS observed_max,
       MAX(abs(value))::DOUBLE AS observed_max_abs
FROM domains
GROUP BY ALL
"""
	if query == "q05":
		sum_bound = first_number(dataset.get("sum_bounds"), 0.0)
		group_bound = int(first_number(dataset.get("c_u"), 5))
		return f"""
WITH per_pu AS (
	SELECT n_name, hash(c_custkey)::UBIGINT AS pu_hash,
	       SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS revenue
	FROM customer, orders, lineitem, supplier, nation, region
	WHERE c_custkey = o_custkey
	  AND l_orderkey = o_orderkey
	  AND l_suppkey = s_suppkey
	  AND c_nationkey = s_nationkey
	  AND s_nationkey = n_nationkey
	  AND n_regionkey = r_regionkey
	  AND r_name = 'ASIA'
	  AND o_orderdate >= CAST('1994-01-01' AS DATE)
	  AND o_orderdate < CAST('1995-01-01' AS DATE)
	GROUP BY n_name, hash(c_custkey)::UBIGINT
), capped AS (
	SELECT * EXCLUDE rn
	FROM (
		SELECT *, row_number() OVER (
			PARTITION BY pu_hash
			ORDER BY hash(pu_hash, n_name)
		) AS rn
		FROM per_pu
	)
	WHERE rn <= {group_bound}
)
SELECT 'q05' AS query, 'revenue' AS aggregate_name, 'contribution' AS bound_kind,
       NULL::DOUBLE AS configured_lower, {sum_bound} AS configured_upper,
       MIN(revenue)::DOUBLE AS observed_min, MAX(revenue)::DOUBLE AS observed_max,
       MAX(abs(revenue))::DOUBLE AS observed_max_abs
FROM capped
"""
	if query == "q06":
		sum_bound = first_number(dataset.get("sum_bounds"), 0.0)
		return f"""
WITH per_pu AS (
	SELECT hash(o_custkey)::UBIGINT AS pu_hash,
	       SUM(l_extendedprice * l_discount)::DOUBLE AS revenue
	FROM lineitem
	JOIN orders ON l_orderkey = o_orderkey
	WHERE l_shipdate >= CAST('1994-01-01' AS DATE)
	  AND l_shipdate < CAST('1995-01-01' AS DATE)
	  AND l_discount BETWEEN 0.05 AND 0.07
	  AND l_quantity < 24
	GROUP BY hash(o_custkey)::UBIGINT
)
SELECT 'q06' AS query, 'revenue' AS aggregate_name, 'contribution' AS bound_kind,
       NULL::DOUBLE AS configured_lower, {sum_bound} AS configured_upper,
       MIN(revenue)::DOUBLE AS observed_min, MAX(revenue)::DOUBLE AS observed_max,
       MAX(abs(revenue))::DOUBLE AS observed_max_abs
FROM per_pu
"""
	if query == "q14":
		sum_bounds = parse_number_list(dataset.get("sum_bound_lists"))
		if len(sum_bounds) != 2:
			sum_bounds = [first_number(dataset.get("sum_bounds"), 0.0)] * 2
		return f"""
WITH per_pu AS (
	SELECT hash(o_custkey)::UBIGINT AS pu_hash,
	       SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)::DOUBLE AS promo_sum_component,
	       SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS total_sum_component
	FROM lineitem
	JOIN orders ON l_orderkey = o_orderkey
	JOIN part ON l_partkey = p_partkey
	WHERE l_shipdate >= CAST('1995-09-01' AS DATE)
	  AND l_shipdate < CAST('1995-10-01' AS DATE)
	GROUP BY hash(o_custkey)::UBIGINT
), values AS (
	SELECT 'promo_sum_component' AS aggregate_name, promo_sum_component AS value, {sum_bounds[0]} AS configured_bound FROM per_pu
	UNION ALL SELECT 'total_sum_component', total_sum_component, {sum_bounds[1]} FROM per_pu
)
SELECT 'q14' AS query, aggregate_name, 'contribution' AS bound_kind,
       NULL::DOUBLE AS configured_lower, configured_bound AS configured_upper,
       MIN(value)::DOUBLE AS observed_min, MAX(value)::DOUBLE AS observed_max,
       MAX(abs(value))::DOUBLE AS observed_max_abs
FROM values
GROUP BY ALL
"""
	if query == "q19":
		sum_bound = first_number(dataset.get("sum_bounds"), 0.0)
		return f"""
WITH per_pu AS (
	SELECT hash(o_custkey)::UBIGINT AS pu_hash,
	       SUM(l_extendedprice * (1 - l_discount))::DOUBLE AS revenue
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
	GROUP BY hash(o_custkey)::UBIGINT
)
SELECT 'q19' AS query, 'revenue' AS aggregate_name, 'contribution' AS bound_kind,
       NULL::DOUBLE AS configured_lower, {sum_bound} AS configured_upper,
       MIN(revenue)::DOUBLE AS observed_min, MAX(revenue)::DOUBLE AS observed_max,
       MAX(abs(revenue))::DOUBLE AS observed_max_abs
FROM per_pu
"""
	raise ValueError(f"unsupported query {query}")


def group_bound_sql(query):
	if query == "q01":
		return """
WITH pu_groups AS (
	SELECT hash(o.o_custkey)::UBIGINT AS pu_hash, l.l_returnflag, l.l_linestatus
	FROM lineitem l
	JOIN orders o ON l.l_orderkey = o.o_orderkey
	WHERE l.l_shipdate <= CAST('1998-09-02' AS DATE)
	GROUP BY ALL
), per_pu AS (
	SELECT pu_hash, COUNT(*) AS group_count
	FROM pu_groups
	GROUP BY pu_hash
)
SELECT 'q01' AS query, MAX(group_count)::DOUBLE AS observed_c_u FROM per_pu
"""
	if query == "q05":
		return """
WITH pu_groups AS (
	SELECT hash(c_custkey)::UBIGINT AS pu_hash, n_name
	FROM customer, orders, lineitem, supplier, nation, region
	WHERE c_custkey = o_custkey
	  AND l_orderkey = o_orderkey
	  AND l_suppkey = s_suppkey
	  AND c_nationkey = s_nationkey
	  AND s_nationkey = n_nationkey
	  AND n_regionkey = r_regionkey
	  AND r_name = 'ASIA'
	  AND o_orderdate >= CAST('1994-01-01' AS DATE)
	  AND o_orderdate < CAST('1995-01-01' AS DATE)
	GROUP BY ALL
), per_pu AS (
	SELECT pu_hash, COUNT(*) AS group_count
	FROM pu_groups
	GROUP BY pu_hash
)
SELECT 'q05' AS query, MAX(group_count)::DOUBLE AS observed_c_u FROM per_pu
"""
	if query in {"q06", "q14", "q19"}:
		return f"SELECT '{query}' AS query, 1.0::DOUBLE AS observed_c_u"
	raise ValueError(f"unsupported query {query}")


def run_duckdb_to_csv(duckdb, db_path, out_path, query, threads, memory_limit, temp_directory, m=None):
	settings = [
		"LOAD privacy",
		"SET priv_rewrite=false",
		"SET preserve_insertion_order=false",
		"SET max_temp_directory_size='12GB'",
		f"SET threads={threads}",
		f"SET memory_limit={sql_quote(memory_limit)}",
	]
	if temp_directory:
		settings.append(f"SET temp_directory={sql_quote(temp_directory)}")
	if m is not None:
		settings.append("SET dp_sample_lanes=1")
		settings.append(f"SET dp_sass_m={m}")
		settings.append("SET dp_sass_rescale=false")
	sql = ";\n".join(settings) + ";\n" + f"COPY ({query}) TO {sql_quote(out_path)} (HEADER, DELIMITER ',');"
	result = subprocess.run([str(duckdb), str(db_path), "-c", sql], text=True, stdout=subprocess.PIPE,
	                        stderr=subprocess.PIPE)
	if result.returncode != 0:
		raise RuntimeError(f"DuckDB failed for {out_path}\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}")


def read_csv(path):
	with open(path, newline="") as handle:
		return list(csv.DictReader(handle))


def write_csv(path, rows, fieldnames):
	with open(path, "w", newline="") as handle:
		writer = csv.DictWriter(handle, fieldnames=fieldnames)
		writer.writeheader()
		for row in rows:
			writer.writerow(row)


def configured_saa_bounds(dataset, aggregate_name):
	if aggregate_name in SUM_AGGREGATES.get(dataset["query_names"][0], []):
		names = SUM_AGGREGATES[dataset["query_names"][0]]
		idx = names.index(aggregate_name)
		return (
			parse_number_list(dataset.get("sass_sum_output_lower_bound_lists"))[idx],
			parse_number_list(dataset.get("sass_sum_output_upper_bound_lists"))[idx],
			"sass_sum_output",
		)
	if aggregate_name in AVG_AGGREGATES.get(dataset["query_names"][0], []):
		names = AVG_AGGREGATES[dataset["query_names"][0]]
		idx = names.index(aggregate_name)
		return (
			parse_number_list(dataset.get("avg_lower_bounds"))[idx],
			parse_number_list(dataset.get("avg_upper_bounds"))[idx],
			"avg_output",
		)
	if aggregate_name in COUNT_AGGREGATES.get(dataset["query_names"][0], []):
		names = COUNT_AGGREGATES[dataset["query_names"][0]]
		idx = names.index(aggregate_name)
		return (
			parse_number_list(dataset.get("sass_count_output_lower_bound_lists"))[idx],
			parse_number_list(dataset.get("sass_count_output_upper_bound_lists"))[idx],
			"sass_count_output",
		)
	return (None, None, "unsupported")


def tolerance(value):
	if value is None:
		return 0.0
	return max(1e-6, abs(value) * 1e-6)


def output_guard(value):
	if value is None:
		return 0.0
	return max(1e-6, abs(value) * 1e-6)


def safe_output_lower(value):
	return None if value is None else value - output_guard(value)


def safe_output_upper(value):
	return None if value is None else value + output_guard(value)


def update_config_bounds(config, summaries):
	points = config_sass_points(config)
	changed = False
	by_point = defaultdict(dict)
	for row in summaries:
		by_point[(row["query"], int(row["m"]))][row["aggregate_name"]] = row
	for key, dataset in points.items():
		query, _m = key
		summary = by_point[key]
		if query in SUM_AGGREGATES:
			lowers = [float(summary[name]["target_lower"]) for name in SUM_AGGREGATES[query]]
			uppers = [float(summary[name]["target_upper"]) for name in SUM_AGGREGATES[query]]
			new_lower = list_setting(lowers)
			new_upper = list_setting(uppers)
			if dataset.get("sass_sum_output_lower_bound_lists") != [new_lower]:
				dataset["sass_sum_output_lower_bound_lists"] = [new_lower]
				changed = True
			if dataset.get("sass_sum_output_upper_bound_lists") != [new_upper]:
				dataset["sass_sum_output_upper_bound_lists"] = [new_upper]
				changed = True
		if query in AVG_AGGREGATES:
			lowers = [float(summary[name]["target_lower"]) for name in AVG_AGGREGATES[query]]
			uppers = [float(summary[name]["target_upper"]) for name in AVG_AGGREGATES[query]]
			if dataset.get("avg_lower_bounds") != lowers:
				dataset["avg_lower_bounds"] = lowers
				changed = True
			if dataset.get("avg_upper_bounds") != uppers:
				dataset["avg_upper_bounds"] = uppers
				changed = True
		if query in COUNT_AGGREGATES:
			lowers = [float(summary[name]["target_lower"]) for name in COUNT_AGGREGATES[query]]
			uppers = [float(summary[name]["target_upper"]) for name in COUNT_AGGREGATES[query]]
			new_lower = list_setting(lowers)
			new_upper = list_setting(uppers)
			if dataset.get("sass_count_output_lower_bound_lists") != [new_lower]:
				dataset["sass_count_output_lower_bound_lists"] = [new_lower]
				changed = True
			if dataset.get("sass_count_output_upper_bound_lists") != [new_upper]:
				dataset["sass_count_output_upper_bound_lists"] = [new_upper]
				changed = True
	return changed


def update_config_group_bounds(config, group_rows):
	observed_by_query = {row["query"]: float(row["observed_c_u"]) for row in group_rows}
	changed = False
	for dataset in config.get("datasets", []):
		query_names = dataset.get("query_names", [])
		if len(query_names) != 1:
			continue
		query = query_names[0]
		if query not in observed_by_query:
			continue
		observed = observed_by_query[query]
		new_c_u = [observed]
		if dataset.get("c_u") != new_c_u:
			dataset["c_u"] = new_c_u
			changed = True
	return changed


def main():
	parser = argparse.ArgumentParser(description="Verify TPC-H SAA central-75 bounds against raw unnoised lane outputs.")
	parser.add_argument("--duckdb", default="build/release/duckdb")
	parser.add_argument("--db", default="tpch_sf30_graviton.db")
	parser.add_argument("--config", required=True)
	parser.add_argument("--out-dir", default="benchmark/dp/tmp/tpch_saa_bound_check")
	parser.add_argument("--threads", type=int, default=16)
	parser.add_argument("--memory-limit", default="16GB")
	parser.add_argument("--temp-directory", default="")
	parser.add_argument("--update-config", action="store_true")
	args = parser.parse_args()

	config_path = Path(args.config)
	config = json.load(open(config_path))
	points = config_sass_points(config)
	out_dir = Path(args.out_dir)
	out_dir.mkdir(parents=True, exist_ok=True)

	raw_rows = []
	contribution_rows = []
	group_rows = []
	for (query, m), dataset in sorted(points.items()):
		print(f"[bounds] sampled lanes {query} m={m}", flush=True)
		tmp_path = out_dir / f"lanes_{query}_m{m}.csv"
		run_duckdb_to_csv(args.duckdb, args.db, str(tmp_path), LANE_SQL[query](m, dataset), args.threads,
		                  args.memory_limit, args.temp_directory, m=m)
		raw_rows.extend(read_csv(tmp_path))

	seen_contrib_queries = set()
	for (query, _m), dataset in sorted(points.items()):
		if query in seen_contrib_queries:
			continue
		seen_contrib_queries.add(query)
		print(f"[bounds] group bound {query}", flush=True)
		group_tmp = out_dir / f"group_bound_{query}.csv"
		run_duckdb_to_csv(args.duckdb, args.db, str(group_tmp), group_bound_sql(query), args.threads,
		                  args.memory_limit, args.temp_directory)
		group_result = read_csv(group_tmp)[0]
		configured_c_u = first_number(dataset.get("c_u"), 1.0)
		observed_c_u = float(group_result["observed_c_u"])
		group_rows.append(
			{
				"query": query,
				"configured_c_u": fmt_number(configured_c_u),
				"observed_c_u": fmt_number(observed_c_u),
				"tightens_config": str(observed_c_u < configured_c_u).lower(),
				"ok": str(configured_c_u >= observed_c_u).lower(),
			}
		)

		print(f"[bounds] contribution bounds {query}", flush=True)
		tmp_path = out_dir / f"contribution_{query}.csv"
		run_duckdb_to_csv(args.duckdb, args.db, str(tmp_path), contribution_sql(query, dataset), args.threads,
		                  args.memory_limit, args.temp_directory)
		contribution_rows.extend(read_csv(tmp_path))

	write_csv(out_dir / "tpch_all5_sampled_unnoised_values.csv", raw_rows,
	          ["query", "m", "group_key", "aggregate_name", "lane", "lane_value"])

	write_csv(
		out_dir / "tpch_all5_group_bounds_check.csv",
		group_rows,
		["query", "configured_c_u", "observed_c_u", "tightens_config", "ok"],
	)

	grouped = defaultdict(list)
	for row in raw_rows:
		value = row["lane_value"]
		if value == "" or value.upper() == "NULL":
			parsed = None
		else:
			parsed = float(value)
		grouped[(row["query"], int(row["m"]), row["aggregate_name"])].append(parsed)

	summaries = []
	for (query, m, aggregate_name), values in sorted(grouped.items()):
		dataset = points[(query, m)]
		configured_lower, configured_upper, kind = configured_saa_bounds(dataset, aggregate_name)
		computed_lower = quantile_cont(values, 0.125)
		computed_upper = quantile_cont(values, 0.875)
		target_lower = safe_output_lower(computed_lower)
		target_upper = safe_output_upper(computed_upper)
		lower_diff = None if configured_lower is None or computed_lower is None else configured_lower - computed_lower
		upper_diff = None if configured_upper is None or computed_upper is None else configured_upper - computed_upper
		needs_update = (
			lower_diff is not None
			and upper_diff is not None
			and (
				abs(configured_lower - target_lower) > tolerance(target_lower)
				or abs(configured_upper - target_upper) > tolerance(target_upper)
			)
		)
		clean = [v for v in values if v is not None and math.isfinite(v)]
		summaries.append(
			{
				"query": query,
				"m": m,
				"aggregate_name": aggregate_name,
				"bound_kind": kind,
				"lane_cells": len(values),
				"non_null_lane_cells": len(clean),
				"observed_min": fmt_number(min(clean) if clean else None),
				"computed_lower": fmt_number(computed_lower),
				"target_lower": fmt_number(target_lower),
				"configured_lower": fmt_number(configured_lower),
				"lower_diff": fmt_number(lower_diff),
				"computed_upper": fmt_number(computed_upper),
				"target_upper": fmt_number(target_upper),
				"configured_upper": fmt_number(configured_upper),
				"upper_diff": fmt_number(upper_diff),
				"observed_max": fmt_number(max(clean) if clean else None),
				"needs_update": str(needs_update).lower(),
			}
		)

	write_csv(
		out_dir / "tpch_all5_saa_bounds_check.csv",
		summaries,
		[
			"query",
			"m",
			"aggregate_name",
			"bound_kind",
			"lane_cells",
			"non_null_lane_cells",
			"observed_min",
			"computed_lower",
			"target_lower",
			"configured_lower",
			"lower_diff",
			"computed_upper",
			"target_upper",
			"configured_upper",
			"upper_diff",
			"observed_max",
			"needs_update",
		],
	)

	for row in contribution_rows:
		lo = row.get("configured_lower")
		hi = row.get("configured_upper")
		observed_min = float(row["observed_min"])
		observed_max = float(row["observed_max"])
		observed_max_abs = float(row["observed_max_abs"])
		if lo:
			ok = observed_min >= float(lo) - tolerance(float(lo)) and observed_max <= float(hi) + tolerance(float(hi))
		else:
			ok = observed_max_abs <= float(hi) + tolerance(float(hi))
		row["ok"] = str(ok).lower()

	write_csv(
		out_dir / "tpch_all5_contribution_bounds_check.csv",
		contribution_rows,
		[
			"query",
			"aggregate_name",
			"bound_kind",
			"configured_lower",
			"configured_upper",
			"observed_min",
			"observed_max",
			"observed_max_abs",
			"ok",
		],
	)

	if args.update_config:
		changed = update_config_bounds(config, summaries)
		changed = update_config_group_bounds(config, group_rows) or changed
		if changed:
			config_path.write_text(json.dumps(config, indent=2) + "\n")
			print(f"[bounds] updated {config_path}", flush=True)
		else:
			print("[bounds] config already matches computed central-75 bounds", flush=True)

	needs = [row for row in summaries if row["needs_update"] == "true"]
	bad_contrib = [row for row in contribution_rows if row["ok"] != "true"]
	bad_group = [row for row in group_rows if row["ok"] != "true"]
	print(f"[bounds] wrote {out_dir}", flush=True)
	print(f"[bounds] SAA bound rows needing update: {len(needs)}", flush=True)
	print(f"[bounds] group-bound rows failing configured c_u: {len(bad_group)}", flush=True)
	print(f"[bounds] contribution/domain rows failing configured bounds: {len(bad_contrib)}", flush=True)
	if needs:
		for row in needs[:20]:
			print(
				f"[bounds] update {row['query']} m={row['m']} {row['aggregate_name']}: "
				f"lower {row['configured_lower']} -> {row['computed_lower']}, "
				f"upper {row['configured_upper']} -> {row['computed_upper']}",
				flush=True,
			)
	if bad_contrib:
		for row in bad_contrib[:20]:
			print(
				f"[bounds] contribution fail {row['query']} {row['aggregate_name']}: "
				f"observed_max_abs={row['observed_max_abs']} configured_upper={row['configured_upper']}",
				flush=True,
			)


if __name__ == "__main__":
	main()
