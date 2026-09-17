-- Preserve SQL's one NULL aggregate row even when a world has no qualifying tuples.
WITH world_results AS (
SELECT o.sample_id, sum(l_extendedprice * l_discount) AS revenue
FROM lineitem l
JOIN benchmark_order_worlds o ON l.l_orderkey = o.o_orderkey
WHERE l_shipdate >= DATE '1994-01-01'
  AND l_shipdate < DATE '1995-01-01'
  AND l_discount BETWEEN 0.05 AND 0.07
  AND l_quantity < 24
GROUP BY o.sample_id
)
SELECT worlds.sample_id, results.revenue
FROM range(64) AS worlds(sample_id)
LEFT JOIN world_results results USING (sample_id)
ORDER BY worlds.sample_id;
