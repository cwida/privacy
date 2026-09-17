-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart, l_shipmode,
           count(*) AS __vpsize,
           sum((CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END)
               / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_shipmode) AS high_line_count,
           sum((CASE WHEN o_orderpriority NOT IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END)
               / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_shipmode) AS low_line_count
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    WHERE l_shipmode IN ('MAIL', 'SHIP')
      AND l_commitdate < l_receiptdate
      AND l_shipdate < l_commitdate
      AND l_receiptdate >= DATE '1994-01-01'
      AND l_receiptdate < DATE '1995-01-01'
    GROUP BY vc.verdict_vpart, l_shipmode
)
SELECT l_shipmode,
       sum(high_line_count * __vpsize) / sum(__vpsize) AS high_line_count,
       stddev_samp(high_line_count) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96
           AS high_line_count_error,
       sum(low_line_count * __vpsize) / sum(__vpsize) AS low_line_count,
       stddev_samp(low_line_count) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96
           AS low_line_count_error
FROM partition_estimates
GROUP BY l_shipmode
ORDER BY l_shipmode;
