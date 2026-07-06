--var:SAMPLES = 64
--var:INDEX_COLS = ['c_custkey']
--var:OUTPUT_COLS = ['revenue']

PREPARE run_query AS
SELECT
    c_custkey,
    c_name,
    SUM(l_extendedprice * (1 - l_discount)) AS revenue,
    c_acctbal,
    n_name,
    c_address,
    c_phone,
    c_comment
FROM
    (SELECT * FROM customer
              JOIN random_samples AS rs ON rs.row_id = customer.rowid
              AND rs.random_binary = TRUE
              AND rs.sample_id = $sample) AS customer,
    orders,
    lineitem,
    nation
WHERE
    c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND c_nationkey = n_nationkey
    AND o_orderdate >= DATE '1993-10-01'
    AND o_orderdate < DATE '1994-01-01'
    AND l_returnflag = 'R'
GROUP BY
    c_custkey,
    c_name,
    c_acctbal,
    c_phone,
    n_name,
    c_address,
    c_comment
ORDER BY
    revenue DESC
LIMIT 20;

EXECUTE run_query(sample := 0);
