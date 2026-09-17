-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH shipping AS (
    SELECT CAST(CAST(1 AS UBIGINT) << CAST(hash(c_custkey) % 64 AS INTEGER) AS UBIGINT) AS membership,
           n1.n_name AS supp_nation, n2.n_name AS cust_nation,
           extract(year FROM l_shipdate) AS l_year,
           l_extendedprice * (1 - l_discount) AS volume
    FROM supplier, lineitem, orders, customer, nation n1, nation n2
    WHERE s_suppkey = l_suppkey
      AND o_orderkey = l_orderkey
      AND c_custkey = o_custkey
      AND s_nationkey = n1.n_nationkey
      AND c_nationkey = n2.n_nationkey
      AND ((n1.n_name = 'FRANCE' AND n2.n_name = 'GERMANY')
        OR (n1.n_name = 'GERMANY' AND n2.n_name = 'FRANCE'))
      AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
), aggregate_lists AS (
    SELECT supp_nation, cust_nation, l_year, as_sum(membership, CAST(volume AS DOUBLE)) AS revenue
    FROM shipping
    GROUP BY supp_nation, cust_nation, l_year
)
SELECT sample_id, supp_nation, cust_nation, l_year,
       list_extract(revenue, sample_id + 1) / 2.0 AS revenue
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(revenue, sample_id + 1) <> 0
ORDER BY sample_id, supp_nation, cust_nation, l_year;
