SELECT cntrycode, as_noised_count(pac_pu) AS numcust,
                  as_noised_sum(pac_pu, c_acctbal) AS totacctbal
FROM (SELECT substring(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal,
             priv_select_gt(priv_hash(hash(c_custkey)),
                           c_acctbal,
                           (SELECT priv_div(as_sum(priv_hash(hash(c_custkey)), c_acctbal),
                                           as_count(priv_hash(hash(c_custkey)), c_acctbal))
                              FROM customer
                             WHERE c_acctbal > 0.00
                               AND substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17'))) AS pac_pu,
        FROM customer
        WHERE substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')
         AND pac_pu <> 0
         AND NOT EXISTS (FROM orders WHERE o_custkey = customer.c_custkey)) AS custsale
 GROUP BY ALL
 ORDER BY ALL;
