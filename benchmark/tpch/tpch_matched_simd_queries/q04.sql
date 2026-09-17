-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT o_orderpriority,
           as_count(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT)) AS order_count
    FROM orders
    WHERE o_orderdate >= DATE '1993-07-01'
      AND o_orderdate < DATE '1993-10-01'
      AND EXISTS (SELECT 1 FROM lineitem
                  WHERE l_orderkey = orders.o_orderkey AND l_commitdate < l_receiptdate)
    GROUP BY o_orderpriority
)
SELECT sample_id, o_orderpriority, list_extract(order_count, sample_id + 1) / 2.0 AS order_count
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(order_count, sample_id + 1) > 0
ORDER BY sample_id, o_orderpriority;
