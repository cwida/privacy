-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH all_nations AS (
    SELECT vc.verdict_vpart, vc.verdict_vprob,
           extract(year FROM o_orderdate) AS o_year,
           l_extendedprice * (1 - l_discount) AS volume,
           n2.n_name AS nation
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON l.l_orderkey = o.o_orderkey
    JOIN part p ON p.p_partkey = l.l_partkey
    JOIN supplier s ON s.s_suppkey = l.l_suppkey
    JOIN nation n1 ON vc.c_nationkey = n1.n_nationkey
    JOIN region r ON n1.n_regionkey = r.r_regionkey
    JOIN nation n2 ON s.s_nationkey = n2.n_nationkey
    WHERE r_name = 'AMERICA'
      AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
      AND p_type = 'ECONOMY ANODIZED STEEL'
), partition_estimates AS (
    SELECT verdict_vpart, o_year,
           count(*) AS __vpsize,
           (sum((CASE WHEN nation = 'BRAZIL' THEN volume ELSE 0 END) / verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY o_year))
           / (sum(volume / verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY o_year)) AS mkt_share
    FROM all_nations
    GROUP BY verdict_vpart, o_year
)
SELECT o_year,
       sum(mkt_share * __vpsize) / sum(__vpsize) AS mkt_share,
       stddev_samp(mkt_share) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS mkt_share_error
FROM partition_estimates
GROUP BY o_year
ORDER BY o_year;
