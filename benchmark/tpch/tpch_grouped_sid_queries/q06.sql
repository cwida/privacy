SELECT c.sample_id, sum(l_extendedprice * l_discount) AS revenue
FROM lineitem l
JOIN orders o ON l.l_orderkey = o.o_orderkey
JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
WHERE l_shipdate >= DATE '1994-01-01'
  AND l_shipdate < DATE '1995-01-01'
  AND l_discount BETWEEN 0.05 AND 0.07
  AND l_quantity < 24
GROUP BY c.sample_id
ORDER BY c.sample_id;
