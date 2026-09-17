-- Preserve SQL's one NULL aggregate row even when a world has no qualifying tuples.
WITH world_results AS (
SELECT o.sample_id,
       100.00 * sum(CASE WHEN p_type LIKE 'PROMO%'
                        THEN l_extendedprice * (1 - l_discount) ELSE 0 END)
              / sum(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM benchmark_order_worlds o
JOIN lineitem l ON o.o_orderkey = l.l_orderkey
JOIN part p ON l.l_partkey = p.p_partkey
WHERE l_shipdate >= DATE '1995-09-01'
  AND l_shipdate < DATE '1995-10-01'
GROUP BY o.sample_id
)
SELECT worlds.sample_id, results.promo_revenue
FROM range(64) AS worlds(sample_id)
LEFT JOIN world_results results USING (sample_id)
ORDER BY worlds.sample_id;
