WITH shipped AS (
    SELECT c.sample_id, l_partkey, l_suppkey, 0.5 * sum(l_quantity) AS half_quantity
    FROM lineitem l
    JOIN orders o ON l.l_orderkey = o.o_orderkey
    JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
    WHERE l_shipdate >= DATE '1994-01-01'
      AND l_shipdate < DATE '1995-01-01'
    GROUP BY c.sample_id, l_partkey, l_suppkey
)
SELECT DISTINCT sh.sample_id, s_name, s_address, 1.0 AS column_0
FROM shipped sh
JOIN partsupp ps ON ps.ps_partkey = sh.l_partkey AND ps.ps_suppkey = sh.l_suppkey
JOIN part p ON p.p_partkey = ps.ps_partkey
JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
WHERE p_name LIKE 'forest%'
  AND ps_availqty > sh.half_quantity
  AND n_name = 'CANADA'
ORDER BY sh.sample_id, s_name;
