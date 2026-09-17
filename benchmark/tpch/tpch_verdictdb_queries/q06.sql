-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart,
           count(*) AS __vpsize,
           sum((l_extendedprice * l_discount) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER () AS revenue
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON l.l_orderkey = o.o_orderkey
    WHERE l_shipdate >= DATE '1994-01-01'
      AND l_shipdate < DATE '1995-01-01'
      AND l_discount BETWEEN 0.05 AND 0.07
      AND l_quantity < 24
    GROUP BY vc.verdict_vpart
)
SELECT sum(revenue * __vpsize) / sum(__vpsize) AS revenue,
       stddev_samp(revenue) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS revenue_error
FROM partition_estimates;
