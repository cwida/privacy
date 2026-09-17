SELECT c.sample_id, n_name,
       sum(l_extendedprice * (1 - l_discount)) AS revenue
FROM benchmark_customer_worlds c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
JOIN supplier s ON l.l_suppkey = s.s_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
JOIN region r ON n.n_regionkey = r.r_regionkey
WHERE c.c_nationkey = s.s_nationkey
  AND r_name = 'ASIA'
  AND o_orderdate >= DATE '1994-01-01'
  AND o_orderdate < DATE '1995-01-01'
GROUP BY c.sample_id, n_name
ORDER BY c.sample_id, revenue DESC;
