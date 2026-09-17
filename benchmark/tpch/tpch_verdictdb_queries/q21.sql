-- WIP rebuttal benchmark; verdict-mode: customer-adapted
-- Fixed CUSTOMER sample. No automatic sample planner or top-k accuracy guarantee.
WITH partition_estimates AS (
 SELECT vc.verdict_vpart, s_name, count(*) AS __vpsize,
 sum(1.0/vc.verdict_vprob)/count(*) * sum(count(*)) OVER (PARTITION BY s_name) AS numwait
 FROM benchmark_verdict_customer vc JOIN orders o ON c_custkey=o_custkey
JOIN lineitem l ON l_orderkey=o_orderkey JOIN supplier s ON s_suppkey=l_suppkey
JOIN nation n ON s_nationkey=n_nationkey
WHERE o_orderstatus='F' AND l_receiptdate>l_commitdate AND n_name='SAUDI ARABIA'
AND EXISTS (SELECT 1 FROM lineitem l2 WHERE l2.l_orderkey=l.l_orderkey AND l2.l_suppkey<>l.l_suppkey)
AND NOT EXISTS (SELECT 1 FROM lineitem l3 WHERE l3.l_orderkey=l.l_orderkey AND l3.l_suppkey<>l.l_suppkey AND l3.l_receiptdate>l3.l_commitdate)
 GROUP BY vc.verdict_vpart, s_name
)
SELECT s_name, sum(numwait*__vpsize)/sum(__vpsize) AS numwait,
 stddev_samp(numwait)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS numwait_error
FROM partition_estimates GROUP BY s_name
ORDER BY numwait DESC, s_name LIMIT 100;
