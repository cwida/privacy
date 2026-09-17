-- WIP rebuttal benchmark; verdict-mode: customer-adapted
-- Fixed CUSTOMER sample. No automatic sample planner or top-k accuracy guarantee.
WITH partition_estimates AS (
 SELECT vc.verdict_vpart, c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, count(*) AS __vpsize,
 sum(l_quantity/vc.verdict_vprob)/count(*) * sum(count(*)) OVER (PARTITION BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice) AS sum_quantity
 FROM benchmark_verdict_customer vc JOIN orders o ON c_custkey=o_custkey
JOIN lineitem l ON l_orderkey=o_orderkey
WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING sum(l_quantity)>300)
 GROUP BY vc.verdict_vpart, c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
)
SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, sum(sum_quantity*__vpsize)/sum(__vpsize) AS "sum(l_quantity)",
 stddev_samp(sum_quantity)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS "sum(l_quantity)_error"
FROM partition_estimates GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
ORDER BY o_totalprice DESC, o_orderdate LIMIT 100;
