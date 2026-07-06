SELECT l_returnflag, l_linestatus,
       as_noised_sum(as_key, l_quantity) AS sum_qty,
       as_noised_sum(as_key, l_extendedprice) AS sum_base_price,
       as_noised_sum(as_key, l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       as_noised_sum(as_key, l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       as_noised_div(as_sum(as_key, l_quantity), as_count(as_key, l_quantity)) AS avg_qty,
       as_noised_div(as_sum(as_key, l_extendedprice), as_count(as_key, l_extendedprice)) AS avg_price,
       as_noised_div(as_sum(as_key, l_discount), as_count(as_key, l_discount)) AS avg_disc,
       as_noised_count(as_key) AS count_order
  FROM (SELECT l_returnflag, l_linestatus, l_quantity, l_extendedprice, l_quantity, l_discount, l_tax,
               priv_hash(hash(o_custkey)) AS as_key
          FROM lineitem JOIN orders ON l_orderkey = o_orderkey
         WHERE l_shipdate <= DATE '1998-09-02') sales
 GROUP BY ALL
 ORDER BY ALL;
