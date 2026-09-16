WITH profit AS (
    SELECT c.sample_id, n_name AS nation, extract(year FROM o_orderdate) AS o_year,
           l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
    FROM part p
    JOIN lineitem l ON p.p_partkey = l.l_partkey
    JOIN supplier s ON s.s_suppkey = l.l_suppkey
    JOIN partsupp ps ON ps.ps_suppkey = l.l_suppkey AND ps.ps_partkey = l.l_partkey
    JOIN orders o ON o.o_orderkey = l.l_orderkey
    JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
    JOIN nation n ON s.s_nationkey = n.n_nationkey
    WHERE p_name LIKE '%green%'
)
SELECT sample_id, nation, o_year, sum(amount) AS sum_profit
FROM profit
GROUP BY sample_id, nation, o_year
ORDER BY sample_id, nation, o_year DESC;
