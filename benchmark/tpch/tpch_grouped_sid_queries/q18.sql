WITH qualifying_orders AS (
    SELECT l_orderkey FROM lineitem
    GROUP BY l_orderkey HAVING sum(l_quantity) > 300
), per_order AS (
    SELECT c.sample_id, c_name, c.c_custkey, o.o_orderkey,
           o_orderdate, o_totalprice, sum(l_quantity) AS sum_quantity
    FROM benchmark_customer_worlds c
    JOIN orders o ON c.c_custkey = o.o_custkey
    JOIN qualifying_orders q ON o.o_orderkey = q.l_orderkey
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    GROUP BY c.sample_id, c_name, c.c_custkey, o.o_orderkey,
             o_orderdate, o_totalprice
), ranked AS (
    SELECT *, row_number() OVER (
        PARTITION BY sample_id ORDER BY o_totalprice DESC, o_orderdate) AS rn
    FROM per_order
)
SELECT sample_id, c_name, c_custkey, o_orderkey, o_orderdate,
       o_totalprice, sum_quantity
FROM ranked WHERE rn <= 100
ORDER BY sample_id, o_totalprice DESC, o_orderdate;
