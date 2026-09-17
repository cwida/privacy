-- WIP rebuttal benchmark; verdict-mode: customer-adapted
-- All orders of a sampled customer are retained: its inner count is exact,
-- including zero orders. Only the outer customer histogram is extrapolated.
WITH customer_counts AS (
 SELECT vc.c_custkey, vc.verdict_vpart, vc.verdict_vprob, count(o_orderkey) AS c_count
 FROM benchmark_verdict_customer vc LEFT JOIN orders o
 ON c_custkey=o_custkey AND o_comment NOT LIKE '%special%requests%'
 GROUP BY vc.c_custkey, vc.verdict_vpart, vc.verdict_vprob
), partition_estimates AS (
 SELECT verdict_vpart, c_count, count(*) AS __vpsize,
 sum(1.0/verdict_vprob)/count(*) * sum(count(*)) OVER (PARTITION BY c_count) AS custdist
 FROM customer_counts GROUP BY verdict_vpart, c_count
)
SELECT c_count, round(sum(custdist*__vpsize)/sum(__vpsize)) AS custdist,
 stddev_samp(custdist)*sqrt(avg(__vpsize))/sqrt(sum(__vpsize))*1.96 AS custdist_error
FROM partition_estimates GROUP BY c_count ORDER BY custdist DESC, c_count DESC;
