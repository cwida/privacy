SELECT c_custkey,
       c_name,
       as_noised_sum(priv_hash(hash(c_custkey)), l_extendedprice * (1 - l_discount)) AS revenue,
       c_acctbal,
       n_name,
       c_address,
       c_phone,
       c_comment
  FROM customer JOIN orders ON c_custkey = o_custkey
                JOIN lineitem ON l_orderkey = o_orderkey
                JOIN nation ON c_nationkey = n_nationkey
 WHERE o_orderdate >= DATE '1993-10-01'
   AND o_orderdate < DATE '1994-01-01'
   AND l_returnflag = 'R'
 GROUP BY ALL
 ORDER BY revenue DESC
 LIMIT 20;
