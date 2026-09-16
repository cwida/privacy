SELECT c.sample_id, sum(l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem l
JOIN part p ON p.p_partkey = l.l_partkey
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
WHERE p_brand = 'Brand#23'
  AND p_container = 'MED BOX'
  AND l_quantity < (SELECT 0.2 * avg(l2.l_quantity)
                    FROM lineitem l2 WHERE l2.l_partkey = p.p_partkey)
GROUP BY c.sample_id
ORDER BY c.sample_id;
