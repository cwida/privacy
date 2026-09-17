-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT n_name AS nation, extract(year FROM o_orderdate) AS o_year,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS DOUBLE)) AS sum_profit,
           as_count(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT)) AS row_count
    FROM part
    JOIN lineitem ON part.p_partkey = lineitem.l_partkey
    JOIN supplier ON supplier.s_suppkey = lineitem.l_suppkey
    JOIN partsupp ON partsupp.ps_partkey = part.p_partkey AND partsupp.ps_suppkey = supplier.s_suppkey
    JOIN orders ON orders.o_orderkey = lineitem.l_orderkey
    JOIN nation ON nation.n_nationkey = supplier.s_nationkey
    WHERE part.p_name LIKE '%green%'
    GROUP BY n_name, o_year
)
SELECT sample_id, nation, o_year, list_extract(sum_profit, sample_id + 1) / 2.0 AS sum_profit
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(row_count, sample_id + 1) > 0
ORDER BY sample_id, nation, o_year DESC;
