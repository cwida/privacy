-- WIP rebuttal benchmark; verdict-mode: artifact-formula
-- Fixed CUSTOMER sample. No automatic sample planner or top-k accuracy guarantee.
WITH partition_estimates AS (
 SELECT vc.verdict_vpart, c_custkey, c_name, c_acctbal, n_name, c_address, c_phone, c_comment, count(*) AS __vpsize,
 sum(l_extendedprice*(1-l_discount)/vc.verdict_vprob)/count(*) * sum(count(*)) OVER (PARTITION BY c_custkey, c_name, c_acctbal, n_name, c_address, c_phone, c_comment) AS revenue
 FROM benchmark_verdict_customer vc JOIN orders o ON c_custkey=o_custkey
JOIN lineitem l ON l_orderkey=o_orderkey JOIN nation n ON c_nationkey=n_nationkey
WHERE o_orderdate >= DATE '1993-10-01' AND o_orderdate < DATE '1994-01-01' AND l_returnflag='R'
 GROUP BY vc.verdict_vpart, c_custkey, c_name, c_acctbal, n_name, c_address, c_phone, c_comment
)
SELECT c_custkey, c_name, c_acctbal, n_name, c_address, c_phone, c_comment, sum(revenue*__vpsize)/sum(__vpsize) AS revenue,
 stddev_samp(revenue)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS revenue_error
FROM partition_estimates GROUP BY c_custkey, c_name, c_acctbal, n_name, c_address, c_phone, c_comment
ORDER BY revenue DESC LIMIT 20;
