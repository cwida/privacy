-- WIP rebuttal benchmark; verdict-mode: artifact-formula
-- Fixed CUSTOMER sample. No automatic sample planner or top-k accuracy guarantee.
WITH partition_estimates AS (
 SELECT vc.verdict_vpart, o_orderkey AS l_orderkey, o_orderdate, o_shippriority, count(*) AS __vpsize,
 sum(l_extendedprice * (1-l_discount) / vc.verdict_vprob) / count(*) * sum(count(*)) OVER (PARTITION BY o_orderkey, o_orderdate, o_shippriority) AS revenue
 FROM benchmark_verdict_customer vc JOIN orders o ON c_custkey=o_custkey
JOIN lineitem l ON l_orderkey=o_orderkey
WHERE c_mktsegment='BUILDING' AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15'
 GROUP BY vc.verdict_vpart, o_orderkey, o_orderdate, o_shippriority
)
SELECT l_orderkey, o_orderdate, o_shippriority, sum(revenue*__vpsize)/sum(__vpsize) AS revenue,
 stddev_samp(revenue)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS revenue_error
FROM partition_estimates GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate LIMIT 10;
