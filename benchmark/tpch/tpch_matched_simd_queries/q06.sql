-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT as_count(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT)) AS world_count,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * l_discount AS DOUBLE)) AS revenue
    FROM lineitem
    JOIN orders ON lineitem.l_orderkey = orders.o_orderkey
    WHERE l_shipdate >= DATE '1994-01-01'
      AND l_shipdate < DATE '1995-01-01'
      AND l_discount BETWEEN 0.05 AND 0.07
      AND l_quantity < 24
)
SELECT sample_id, CASE WHEN list_extract(world_count, sample_id + 1) > 0
                       THEN list_extract(revenue, sample_id + 1) / 2.0 ELSE NULL END AS revenue
FROM aggregate_lists, range(64) AS worlds(sample_id)
ORDER BY sample_id;
