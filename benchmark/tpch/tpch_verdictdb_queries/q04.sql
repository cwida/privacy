-- WIP rebuttal benchmark; verdict-mode: customer-adapted
-- Exact EXISTS on dependent orders; manual adaptation, not native approximation support.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart, o_orderpriority,
           count(*) AS __vpsize,
           sum(1.0 / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY o_orderpriority) AS order_count
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    WHERE o_orderdate >= DATE '1993-07-01'
      AND o_orderdate < DATE '1993-10-01'
      AND EXISTS (SELECT 1 FROM lineitem l
                  WHERE l.l_orderkey = o.o_orderkey
                    AND l.l_commitdate < l.l_receiptdate)
    GROUP BY vc.verdict_vpart, o_orderpriority
)
SELECT o_orderpriority,
       round(sum(order_count * __vpsize) / sum(__vpsize)) AS order_count,
       stddev_samp(order_count) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS order_count_error
FROM partition_estimates
GROUP BY o_orderpriority
ORDER BY o_orderpriority;
