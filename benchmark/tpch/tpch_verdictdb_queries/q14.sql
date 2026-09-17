-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart,
           count(*) AS __vpsize,
           100.0
           * (sum((CASE WHEN p_type LIKE 'PROMO%'
                        THEN l_extendedprice * (1 - l_discount) ELSE 0 END) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER ())
           / (sum((l_extendedprice * (1 - l_discount)) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER ()) AS promo_revenue
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    JOIN part p ON l.l_partkey = p.p_partkey
    WHERE l_shipdate >= DATE '1995-09-01'
      AND l_shipdate < DATE '1995-10-01'
    GROUP BY vc.verdict_vpart
)
SELECT sum(promo_revenue * __vpsize) / sum(__vpsize) AS promo_revenue,
       stddev_samp(promo_revenue) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS promo_revenue_error
FROM partition_estimates;
