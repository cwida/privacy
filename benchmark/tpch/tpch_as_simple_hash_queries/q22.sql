SELECT cntrycode, as_noised_count(priv_hash(hash(c_custkey)), 1) AS numcust,
                  as_noised_sum(priv_hash(hash(c_custkey)), c_acctbal) AS totacctbal
  FROM (SELECT substring(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal, c_custkey
          FROM customer
         WHERE substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')
           AND c_acctbal > (SELECT as_noised_div(as_sum(priv_hash(hash(c_custkey)), c_acctbal),
                                                  as_count(priv_hash(hash(c_custkey)), c_acctbal))
                              FROM customer
                             WHERE c_acctbal > 0.00
                               AND substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17'))
           AND NOT EXISTS (FROM orders WHERE o_custkey = c_custkey)) AS custsale
 GROUP BY ALL
 ORDER BY ALL;
