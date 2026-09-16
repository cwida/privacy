WITH per_customer AS (
    SELECT c.sample_id, c.c_custkey, c_name,
           sum(l_extendedprice * (1 - l_discount)) AS revenue,
           c_acctbal, n_name, c_address, c_phone, c_comment
    FROM benchmark_customer_worlds c
    JOIN orders o ON c.c_custkey = o.o_custkey
    JOIN lineitem l ON l.l_orderkey = o.o_orderkey
    JOIN nation n ON c.c_nationkey = n.n_nationkey
    WHERE o_orderdate >= DATE '1993-10-01'
      AND o_orderdate < DATE '1994-01-01'
      AND l_returnflag = 'R'
    GROUP BY c.sample_id, c.c_custkey, c_name, c_acctbal, n_name,
             c_address, c_phone, c_comment
), ranked AS (
    SELECT *, row_number() OVER (
        PARTITION BY sample_id ORDER BY revenue DESC) AS rn
    FROM per_customer
)
SELECT sample_id, c_custkey, c_name, revenue, c_acctbal, n_name,
       c_address, c_phone, c_comment
FROM ranked WHERE rn <= 20
ORDER BY sample_id, revenue DESC;
