WITH per_order AS (
    SELECT c.sample_id, l_orderkey,
           sum(l_extendedprice * (1 - l_discount)) AS revenue,
           o_orderdate, o_shippriority
    FROM benchmark_customer_worlds c
    JOIN orders o ON c.c_custkey = o.o_custkey
    JOIN lineitem l ON l.l_orderkey = o.o_orderkey
    WHERE c_mktsegment = 'BUILDING'
      AND o_orderdate < DATE '1995-03-15'
      AND l_shipdate > DATE '1995-03-15'
    GROUP BY c.sample_id, l_orderkey, o_orderdate, o_shippriority
), ranked AS (
    SELECT *, row_number() OVER (
        PARTITION BY sample_id ORDER BY revenue DESC, o_orderdate) AS rn
    FROM per_order
)
SELECT sample_id, l_orderkey, revenue, o_orderdate, o_shippriority
FROM ranked WHERE rn <= 10
ORDER BY sample_id, revenue DESC, o_orderdate;
