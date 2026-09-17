-- WIP rebuttal benchmark: SIMD aggregation over the exact one-hot customer worlds used by Disjoint-SID.
WITH aggregate_lists AS (
    SELECT l_returnflag, l_linestatus,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_quantity AS DOUBLE)) AS sum_qty,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice AS DOUBLE)) AS sum_base_price,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * (1 - l_discount) AS DOUBLE)) AS sum_disc_price,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice * (1 - l_discount) * (1 + l_tax) AS DOUBLE)) AS sum_charge,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_quantity AS DOUBLE)) AS avg_qty_sum,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_extendedprice AS DOUBLE)) AS avg_price_sum,
           as_sum(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT),
                  CAST(l_discount AS DOUBLE)) AS avg_disc_sum,
           as_count(CAST(CAST(1 AS UBIGINT) << CAST(hash(o_custkey) % 64 AS INTEGER) AS UBIGINT)) AS row_count
    FROM lineitem
    JOIN orders ON l_orderkey = o_orderkey
    WHERE l_shipdate <= DATE '1998-09-02'
    GROUP BY l_returnflag, l_linestatus
)
SELECT sample_id, l_returnflag, l_linestatus,
       list_extract(sum_qty, sample_id + 1) / 2.0 AS sum_qty,
       list_extract(sum_base_price, sample_id + 1) / 2.0 AS sum_base_price,
       list_extract(sum_disc_price, sample_id + 1) / 2.0 AS sum_disc_price,
       list_extract(sum_charge, sample_id + 1) / 2.0 AS sum_charge,
       list_extract(avg_qty_sum, sample_id + 1) / list_extract(row_count, sample_id + 1) AS avg_qty,
       list_extract(avg_price_sum, sample_id + 1) / list_extract(row_count, sample_id + 1) AS avg_price,
       list_extract(avg_disc_sum, sample_id + 1) / list_extract(row_count, sample_id + 1) AS avg_disc,
       list_extract(row_count, sample_id + 1) / 2.0 AS count_order
FROM aggregate_lists, range(64) AS worlds(sample_id)
WHERE list_extract(row_count, sample_id + 1) > 0
ORDER BY sample_id, l_returnflag, l_linestatus;
