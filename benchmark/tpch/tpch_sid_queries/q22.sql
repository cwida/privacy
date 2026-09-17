WITH positive_avg AS (
    SELECT sample_id, avg(c_acctbal) AS avg_acctbal
    FROM benchmark_customer_worlds
    WHERE c_acctbal > 0.00
      AND substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')
    GROUP BY sample_id
), eligible AS (
    SELECT c.sample_id, substring(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
    FROM benchmark_customer_worlds c
    JOIN positive_avg a ON c.sample_id = a.sample_id
    WHERE substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')
      AND c.c_acctbal > a.avg_acctbal
      AND NOT EXISTS (SELECT 1 FROM orders o WHERE o.o_custkey = c.c_custkey)
)
SELECT sample_id, cntrycode, count(*) AS numcust, sum(c_acctbal) AS totacctbal
FROM eligible
GROUP BY sample_id, cntrycode
ORDER BY sample_id, cntrycode;
