-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT n_name,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(c_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * (1 - l_discount) AS DOUBLE)) AS revenue
    FROM customer
    JOIN orders ON c_custkey = o_custkey
    JOIN lineitem ON o_orderkey = l_orderkey
    JOIN supplier ON l_suppkey = s_suppkey
    JOIN nation ON s_nationkey = n_nationkey AND c_nationkey = s_nationkey
    JOIN region ON n_regionkey = r_regionkey
    WHERE r_name = 'ASIA'
      AND o_orderdate >= DATE '1994-01-01'
      AND o_orderdate < DATE '1995-01-01'
    GROUP BY n_name
)
SELECT sample_id, n_name, list_extract(revenue, sample_id + 1) / 2.0 AS revenue
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(revenue, sample_id + 1) <> 0
ORDER BY sample_id, revenue DESC;
