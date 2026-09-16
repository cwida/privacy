WITH all_nations AS (
    SELECT c.sample_id, extract(year FROM o_orderdate) AS o_year,
           l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation
    FROM part p
    JOIN lineitem l ON p.p_partkey = l.l_partkey
    JOIN supplier s ON s.s_suppkey = l.l_suppkey
    JOIN orders o ON l.l_orderkey = o.o_orderkey
    JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
    JOIN nation n1 ON c.c_nationkey = n1.n_nationkey
    JOIN region r ON n1.n_regionkey = r.r_regionkey
    JOIN nation n2 ON s.s_nationkey = n2.n_nationkey
    WHERE r_name = 'AMERICA'
      AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
      AND p_type = 'ECONOMY ANODIZED STEEL'
)
SELECT sample_id, o_year,
       sum(CASE WHEN nation = 'BRAZIL' THEN volume ELSE 0 END) / sum(volume) AS mkt_share
FROM all_nations
GROUP BY sample_id, o_year
ORDER BY sample_id, o_year;
