-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart, n_name,
           count(*) AS __vpsize,
           sum((l_extendedprice * (1 - l_discount)) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY n_name) AS revenue
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON l.l_orderkey = o.o_orderkey
    JOIN supplier s ON l.l_suppkey = s.s_suppkey
    JOIN nation n ON s.s_nationkey = n.n_nationkey
    JOIN region r ON n.n_regionkey = r.r_regionkey
    WHERE vc.c_nationkey = s.s_nationkey
      AND r_name = 'ASIA'
      AND o_orderdate >= DATE '1994-01-01'
      AND o_orderdate < DATE '1995-01-01'
    GROUP BY vc.verdict_vpart, n_name
)
SELECT n_name,
       sum(revenue * __vpsize) / sum(__vpsize) AS revenue,
       stddev_samp(revenue) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS revenue_error
FROM partition_estimates
GROUP BY n_name
ORDER BY revenue DESC;
