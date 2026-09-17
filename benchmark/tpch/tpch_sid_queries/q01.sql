SELECT o.sample_id, l_returnflag, l_linestatus,
       sum(l_quantity) AS sum_qty,
       sum(l_extendedprice) AS sum_base_price,
       sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price,
       avg(l_discount) AS avg_disc, count(*) AS count_order
FROM lineitem l
JOIN benchmark_order_worlds o ON l.l_orderkey = o.o_orderkey
WHERE l_shipdate <= DATE '1998-09-02'
GROUP BY o.sample_id, l_returnflag, l_linestatus
ORDER BY o.sample_id, l_returnflag, l_linestatus;
