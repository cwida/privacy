-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH profit AS (
    SELECT vc.verdict_vpart, vc.verdict_vprob, n_name AS nation,
           extract(year FROM o_orderdate) AS o_year,
           l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    JOIN part p ON p.p_partkey = l.l_partkey
    JOIN supplier s ON s.s_suppkey = l.l_suppkey
    JOIN partsupp ps ON ps.ps_suppkey = l.l_suppkey AND ps.ps_partkey = l.l_partkey
    JOIN nation n ON s.s_nationkey = n.n_nationkey
    WHERE p_name LIKE '%green%'
), partition_estimates AS (
    SELECT verdict_vpart, nation, o_year,
           count(*) AS __vpsize,
           sum(amount / verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY nation, o_year) AS sum_profit
    FROM profit
    GROUP BY verdict_vpart, nation, o_year
)
SELECT nation, o_year,
       sum(sum_profit * __vpsize) / sum(__vpsize) AS sum_profit,
       stddev_samp(sum_profit) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS sum_profit_error
FROM partition_estimates
GROUP BY nation, o_year
ORDER BY nation, o_year DESC;
