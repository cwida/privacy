-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT as_count(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT)) AS world_count,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * (1 - l_discount) AS DOUBLE)) AS revenue
    FROM lineitem
    JOIN part ON lineitem.l_partkey = part.p_partkey
    JOIN orders ON lineitem.l_orderkey = orders.o_orderkey
    WHERE ((p_brand = 'Brand#12'
            AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
            AND l_quantity BETWEEN 1 AND 11 AND p_size BETWEEN 1 AND 5)
        OR (p_brand = 'Brand#23'
            AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
            AND l_quantity BETWEEN 10 AND 20 AND p_size BETWEEN 1 AND 10)
        OR (p_brand = 'Brand#34'
            AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
            AND l_quantity BETWEEN 20 AND 30 AND p_size BETWEEN 1 AND 15))
      AND l_shipmode IN ('AIR', 'AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON'
)
SELECT sample_id, CASE WHEN list_extract(world_count, sample_id + 1) > 0
                       THEN list_extract(revenue, sample_id + 1) / 2.0 ELSE NULL END AS revenue
FROM aggregate_lists, range(64) AS worlds(sample_id)
ORDER BY sample_id;
