-- Preserve SQL's one NULL aggregate row even when a world has no qualifying tuples.
WITH world_results AS (
SELECT o.sample_id, sum(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem l
JOIN part p ON p.p_partkey = l.l_partkey
JOIN benchmark_order_worlds o ON o.o_orderkey = l.l_orderkey
WHERE ((p_brand = 'Brand#12'
        AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
        AND l_quantity BETWEEN 1 AND 11 AND p_size BETWEEN 1 AND 5)
    OR (p_brand = 'Brand#23'
        AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
        AND l_quantity BETWEEN 10 AND 20 AND p_size BETWEEN 1 AND 10)
    OR (p_brand = 'Brand#34'
        AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
        AND l_quantity BETWEEN 20 AND 30 AND p_size BETWEEN 1 AND 15))
  AND l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON'
GROUP BY o.sample_id
)
SELECT worlds.sample_id, results.revenue
FROM range(64) AS worlds(sample_id)
LEFT JOIN world_results results USING (sample_id)
ORDER BY worlds.sample_id;
