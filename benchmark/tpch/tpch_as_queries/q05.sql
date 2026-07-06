SELECT n_name, as_noised_sum(priv_hash(hash(c_custkey)), l_extendedprice * (1 - l_discount)) AS revenue
  FROM customer JOIN orders ON c_custkey = o_custkey
                JOIN lineitem ON o_orderkey = l_orderkey
                JOIN supplier ON l_suppkey = s_suppkey
                JOIN nation ON s_nationkey = n_nationkey AND c_nationkey = s_nationkey
                JOIN region ON n_regionkey = r_regionkey
 WHERE r_name = 'ASIA' AND o_orderdate >= DATE '1994-01-01' AND o_orderdate <  DATE '1995-01-01'
 GROUP BY ALL
 ORDER BY revenue DESC;
