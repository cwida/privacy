-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT l_shipmode,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END AS DOUBLE))
               AS high_line_count,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(CASE WHEN o_orderpriority NOT IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END AS DOUBLE))
               AS low_line_count
    FROM orders
    JOIN lineitem ON o_orderkey = l_orderkey
    WHERE l_commitdate < l_receiptdate
      AND l_shipdate < l_commitdate
      AND l_shipmode IN ('MAIL', 'SHIP')
      AND l_receiptdate >= DATE '1994-01-01'
      AND l_receiptdate < DATE '1995-01-01'
    GROUP BY l_shipmode
)
SELECT sample_id, l_shipmode,
       list_extract(high_line_count, sample_id + 1) / 2.0 AS high_line_count,
       list_extract(low_line_count, sample_id + 1) / 2.0 AS low_line_count
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(high_line_count, sample_id + 1) <> 0
   OR list_extract(low_line_count, sample_id + 1) <> 0
ORDER BY sample_id, l_shipmode;
