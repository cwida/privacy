-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH all_nations AS (
    SELECT CAST(CAST(1 AS UBIGINT) << CAST(hash(c_custkey) % 64 AS INTEGER) AS UBIGINT) AS membership,
           extract(year FROM o_orderdate) AS o_year,
           l_extendedprice * (1 - l_discount) AS volume,
           n2.n_name AS nation
    FROM part
    JOIN lineitem ON p_partkey = l_partkey
    JOIN orders ON l_orderkey = o_orderkey
    JOIN customer ON o_custkey = c_custkey
    JOIN nation n1 ON c_nationkey = n1.n_nationkey
    JOIN region ON n1.n_regionkey = r_regionkey
    JOIN supplier ON l_suppkey = s_suppkey
    JOIN nation n2 ON s_nationkey = n2.n_nationkey
    WHERE r_name = 'AMERICA'
      AND p_type = 'ECONOMY ANODIZED STEEL'
      AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
), aggregate_lists AS (
    SELECT o_year,
           as_sum(membership, CAST(CASE WHEN nation = 'BRAZIL' THEN volume ELSE 0 END AS DOUBLE)) AS brazil_volume,
           as_sum(membership, CAST(volume AS DOUBLE)) AS total_volume
    FROM all_nations
    GROUP BY o_year
)
SELECT sample_id, o_year,
       list_extract(brazil_volume, sample_id + 1) / list_extract(total_volume, sample_id + 1) AS mkt_share
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(total_volume, sample_id + 1) <> 0
ORDER BY sample_id, o_year;
