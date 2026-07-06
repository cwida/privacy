--var:SAMPLES = 64
--var:INDEX_COLS = ['l_orderkey', 'o_orderdate', 'o_shippriority']
--var:OUTPUT_COLS = ['revenue']

PREPARE run_query AS
SELECT
    l_orderkey,
    SUM(l_extendedprice * (1 - l_discount)) AS revenue,
    o_orderdate,
    o_shippriority
FROM
    (SELECT * FROM customer
              JOIN random_samples AS rs ON rs.row_id = customer.rowid
              AND rs.random_binary = TRUE
              AND rs.sample_id = $sample) AS customer,
    orders,
    lineitem
WHERE
    c_mktsegment = 'BUILDING'
    AND c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND o_orderdate < DATE '1995-03-15'
    AND l_shipdate > DATE '1995-03-15'
GROUP BY
    l_orderkey,
    o_orderdate,
    o_shippriority
ORDER BY
    revenue DESC,
    o_orderdate
LIMIT 10;

EXECUTE run_query(sample := 0);
