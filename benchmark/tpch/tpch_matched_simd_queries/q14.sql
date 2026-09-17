-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT as_count(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT)) AS world_count,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END AS DOUBLE))
               AS promo,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * (1 - l_discount) AS DOUBLE)) AS total
    FROM lineitem
    JOIN part ON l_partkey = p_partkey
    JOIN orders ON l_orderkey = o_orderkey
    WHERE l_shipdate >= DATE '1995-09-01'
      AND l_shipdate < DATE '1995-10-01'
)
SELECT sample_id, CASE WHEN list_extract(world_count, sample_id + 1) > 0
                       THEN 100.0 * list_extract(promo, sample_id + 1) / list_extract(total, sample_id + 1) ELSE NULL END AS promo_revenue
FROM aggregate_lists, range(64) AS worlds(sample_id)
ORDER BY sample_id;
