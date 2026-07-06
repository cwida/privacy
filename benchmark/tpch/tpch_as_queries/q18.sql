SELECT c_name,
       c_custkey,
       o_orderkey,
       o_orderdate,
       o_totalprice,
       as_noised_sum(priv_hash(hash(c_custkey)), l_quantity) AS sum_quantity
  FROM customer JOIN orders ON c_custkey = o_custkey
                JOIN lineitem ON o_orderkey = l_orderkey
 WHERE o_orderkey IN (SELECT l_orderkey
                        FROM lineitem
                       GROUP BY l_orderkey
                      HAVING SUM(l_quantity) > 300)
 GROUP BY ALL
 ORDER BY o_totalprice DESC, o_orderdate
 LIMIT 100;
