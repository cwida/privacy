-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH shipping AS (
    SELECT vc.verdict_vpart, vc.verdict_vprob,
           n1.n_name AS supp_nation, n2.n_name AS cust_nation,
           extract(year FROM l_shipdate) AS l_year,
           l_extendedprice * (1 - l_discount) AS volume
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    JOIN supplier s ON s.s_suppkey = l.l_suppkey
    JOIN nation n1 ON s.s_nationkey = n1.n_nationkey
    JOIN nation n2 ON vc.c_nationkey = n2.n_nationkey
    WHERE ((n1.n_name = 'FRANCE' AND n2.n_name = 'GERMANY')
        OR (n1.n_name = 'GERMANY' AND n2.n_name = 'FRANCE'))
      AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
), partition_estimates AS (
    SELECT verdict_vpart, supp_nation, cust_nation, l_year,
           count(*) AS __vpsize,
           sum(volume / verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY supp_nation, cust_nation, l_year) AS revenue
    FROM shipping
    GROUP BY verdict_vpart, supp_nation, cust_nation, l_year
)
SELECT supp_nation, cust_nation, l_year,
       sum(revenue * __vpsize) / sum(__vpsize) AS revenue,
       stddev_samp(revenue) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS revenue_error
FROM partition_estimates
GROUP BY supp_nation, cust_nation, l_year
ORDER BY supp_nation, cust_nation, l_year;
