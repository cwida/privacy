SELECT o.sample_id,
       100.00 * sum(CASE WHEN p_type LIKE 'PROMO%'
                        THEN l_extendedprice * (1 - l_discount) ELSE 0 END)
              / sum(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM benchmark_order_worlds o
JOIN customer c ON o.o_custkey = c.c_custkey
JOIN lineitem l ON o.o_orderkey = l.l_orderkey
JOIN part p ON l.l_partkey = p.p_partkey
WHERE l_shipdate >= DATE '1995-09-01'
  AND l_shipdate < DATE '1995-10-01'
GROUP BY o.sample_id
ORDER BY o.sample_id;
