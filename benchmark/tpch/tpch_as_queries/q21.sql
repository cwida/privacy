SELECT s_name, as_noised_count(priv_hash(hash(o_custkey))) AS numwait
  FROM supplier JOIN lineitem l1 ON s_suppkey = l1.l_suppkey
                JOIN orders ON o_orderkey = l1.l_orderkey
                JOIN nation ON s_nationkey = n_nationkey
 WHERE o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND n_name = 'SAUDI ARABIA'
   -- The outer orders join supplies an order row for both correlated existence checks.
   AND EXISTS (SELECT 1 FROM lineitem l2
                WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
   AND NOT EXISTS (SELECT 1 FROM lineitem l3
                    WHERE l3.l_orderkey = l1.l_orderkey
                      AND l3.l_suppkey <> l1.l_suppkey
                      AND l3.l_receiptdate > l3.l_commitdate)
GROUP BY s_name
ORDER BY numwait DESC, s_name
LIMIT 100;
