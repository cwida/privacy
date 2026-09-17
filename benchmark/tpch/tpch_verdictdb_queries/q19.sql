-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart,
           count(*) AS __vpsize,
           sum((l_extendedprice * (1 - l_discount)) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER () AS revenue
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    JOIN part p ON p.p_partkey = l.l_partkey
    WHERE ((p_brand = 'Brand#12'
            AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
            AND l_quantity BETWEEN 1 AND 11 AND p_size BETWEEN 1 AND 5)
        OR (p_brand = 'Brand#23'
            AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
            AND l_quantity BETWEEN 10 AND 20 AND p_size BETWEEN 1 AND 10)
        OR (p_brand = 'Brand#34'
            AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
            AND l_quantity BETWEEN 20 AND 30 AND p_size BETWEEN 1 AND 15))
      AND l_shipmode IN ('AIR', 'AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON'
    GROUP BY vc.verdict_vpart
)
SELECT sum(revenue * __vpsize) / sum(__vpsize) AS revenue,
       stddev_samp(revenue) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS revenue_error
FROM partition_estimates;
