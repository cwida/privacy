WITH shipping AS (
    SELECT c.sample_id, n1.n_name AS supp_nation, n2.n_name AS cust_nation,
           extract(year FROM l_shipdate) AS l_year,
           l_extendedprice * (1 - l_discount) AS volume
    FROM supplier s
    JOIN lineitem l ON s.s_suppkey = l.l_suppkey
    JOIN orders o ON o.o_orderkey = l.l_orderkey
    JOIN benchmark_customer_worlds c ON c.c_custkey = o.o_custkey
    JOIN nation n1 ON s.s_nationkey = n1.n_nationkey
    JOIN nation n2 ON c.c_nationkey = n2.n_nationkey
    WHERE ((n1.n_name = 'FRANCE' AND n2.n_name = 'GERMANY')
        OR (n1.n_name = 'GERMANY' AND n2.n_name = 'FRANCE'))
      AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
)
SELECT sample_id, supp_nation, cust_nation, l_year, sum(volume) AS revenue
FROM shipping
GROUP BY sample_id, supp_nation, cust_nation, l_year
ORDER BY sample_id, supp_nation, cust_nation, l_year;
