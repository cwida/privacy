--var:SAMPLES = 128
--var:INDEX_COLS = ['c_custkey', 'o_orderkey']
--var:OUTPUT_COLS = ['sum_quantity']

PREPARE run_query AS
WITH customer_sample AS (
    SELECT * FROM customer
    JOIN random_samples AS rs ON rs.row_id = customer.rowid
    AND rs.random_binary = TRUE
    AND rs.sample_id = $sample
)
SELECT
    c_name,
    c_custkey,
    o_orderkey,
    o_orderdate,
    o_totalprice,
    SUM(l_quantity) AS sum_quantity
FROM
    customer_sample,
    orders,
    lineitem
WHERE
    o_orderkey IN (
        SELECT
            l_orderkey
        FROM
            lineitem
        GROUP BY
            l_orderkey
        HAVING
            SUM(l_quantity) > 300
    )
    AND c_custkey = o_custkey
    AND o_orderkey = l_orderkey
GROUP BY
    c_name,
    c_custkey,
    o_orderkey,
    o_orderdate,
    o_totalprice
ORDER BY
    o_totalprice DESC,
    o_orderdate
LIMIT 100;

EXECUTE run_query(sample := 0);
