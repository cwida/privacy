-- WIP rebuttal benchmark; verdict-mode: customer-adapted
-- Partition-dependent average; exact anti-existence test is valid because all
-- orders of each selected CUSTOMER are retained. This is a manual adaptation.
WITH thresholds AS (
 SELECT verdict_vpart, sum(c_acctbal/verdict_vprob)/sum(1.0/verdict_vprob) AS threshold
 FROM benchmark_verdict_customer
 WHERE c_acctbal>0 AND substring(c_phone,1,2) IN ('13','31','23','29','30','18','17')
 GROUP BY verdict_vpart
), eligible AS (
 SELECT c.*, substring(c_phone,1,2) AS cntrycode
 FROM benchmark_verdict_customer c JOIN thresholds t ON c.verdict_vpart=t.verdict_vpart
 WHERE substring(c_phone,1,2) IN ('13','31','23','29','30','18','17')
 AND c_acctbal>t.threshold
 AND NOT EXISTS (SELECT 1 FROM orders o WHERE o.o_custkey=c.c_custkey)
), partition_estimates AS (
 SELECT verdict_vpart, cntrycode, count(*) AS __vpsize,
 sum(1.0/verdict_vprob)/count(*) * sum(count(*)) OVER (PARTITION BY cntrycode) AS numcust,
 sum(c_acctbal/verdict_vprob)/count(*) * sum(count(*)) OVER (PARTITION BY cntrycode) AS totacctbal
 FROM eligible GROUP BY verdict_vpart, cntrycode
)
SELECT cntrycode, round(sum(numcust*__vpsize)/sum(__vpsize)) AS numcust,
 stddev_samp(numcust)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS numcust_error,
 sum(totacctbal*__vpsize)/sum(__vpsize) AS totacctbal,
 stddev_samp(totacctbal)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS totacctbal_error
FROM partition_estimates GROUP BY cntrycode ORDER BY cntrycode;
