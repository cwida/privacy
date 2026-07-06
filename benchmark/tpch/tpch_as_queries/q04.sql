SELECT o_orderpriority, as_noised_count(priv_hash(hash(o_custkey))) AS order_count
  FROM orders
 WHERE o_orderdate >= DATE '1993-07-01' AND o_orderdate <  DATE '1993-10-01'
   AND EXISTS (FROM lineitem
              WHERE l_orderkey = orders.o_orderkey AND l_commitdate < l_receiptdate)
 GROUP BY ALL
 ORDER BY ALL;
