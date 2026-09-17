-- Preserve SQL's one NULL aggregate row even when a world has no qualifying tuples.
WITH world_results AS (
SELECT c.sample_id, sum(l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem l
JOIN part p ON p.p_partkey = l.l_partkey
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
WHERE p_brand = 'Brand#23'
  AND p_container = 'MED BOX'
  AND l_quantity < (SELECT 0.2 * avg(l2.l_quantity)
                    FROM lineitem l2
                    JOIN benchmark_order_worlds o2 ON o2.o_orderkey = l2.l_orderkey
                    WHERE l2.l_partkey = p.p_partkey AND o2.sample_id = c.sample_id)
GROUP BY c.sample_id
)
SELECT worlds.sample_id, results.avg_yearly
FROM range(64) AS worlds(sample_id)
LEFT JOIN world_results results USING (sample_id)
ORDER BY worlds.sample_id;
