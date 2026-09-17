-- WIP rebuttal benchmark; verdict-mode: customer-adapted
-- Flattened aggregate comparison, evaluated inside each customer partition.
-- This manual cluster-sampling adaptation is not a captured VerdictDB rewrite.
WITH sampled_lines AS (
 SELECT l.*, vc.verdict_vpart, vc.verdict_vprob
 FROM benchmark_verdict_customer vc JOIN orders o ON c_custkey=o_custkey
 JOIN lineitem l ON l_orderkey=o_orderkey
), thresholds AS (
 SELECT verdict_vpart, l_partkey,
  0.2*sum(l_quantity/verdict_vprob)/sum(1.0/verdict_vprob) AS threshold
 FROM sampled_lines GROUP BY verdict_vpart, l_partkey
), partition_estimates AS (
 SELECT l.verdict_vpart, count(*) AS __vpsize,
 sum(l_extendedprice/l.verdict_vprob)/count(*) * sum(count(*)) OVER () / 7.0 AS avg_yearly
 FROM sampled_lines l JOIN thresholds t
 ON l.verdict_vpart=t.verdict_vpart AND l.l_partkey=t.l_partkey
 JOIN part p ON p_partkey=l.l_partkey
 WHERE p_brand='Brand#23' AND p_container='MED BOX' AND l_quantity<t.threshold
 GROUP BY l.verdict_vpart
)
SELECT sum(avg_yearly*__vpsize)/sum(__vpsize) AS avg_yearly,
 stddev_samp(avg_yearly)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS avg_yearly_error
FROM partition_estimates;
