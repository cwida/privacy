SELECT o.sample_id, o_orderpriority, count(*) AS order_count
FROM benchmark_order_worlds o
JOIN customer c ON c.c_custkey = o.o_custkey
WHERE o_orderdate >= DATE '1993-07-01'
  AND o_orderdate < DATE '1993-10-01'
  AND EXISTS (SELECT 1 FROM lineitem l
              WHERE l.l_orderkey = o.o_orderkey
                AND l.l_commitdate < l.l_receiptdate)
GROUP BY o.sample_id, o_orderpriority
ORDER BY o.sample_id, o_orderpriority;
