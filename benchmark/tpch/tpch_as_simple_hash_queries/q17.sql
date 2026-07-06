SELECT as_noised_sum(priv_hash(hash(l_orderkey)), l_extendedprice) / 7.0 AS avg_yearly
  FROM lineitem JOIN part ON lineitem.l_partkey = part.p_partkey
 WHERE part.p_brand = 'Brand#23' AND part.p_container = 'MED BOX' AND
       lineitem.l_quantity*5 < (SELECT as_noised_div(as_sum(priv_hash(hash(l_sub.l_orderkey)), l_sub.l_quantity),
                                                      as_count(priv_hash(hash(l_sub.l_orderkey)), l_sub.l_quantity))
                                  FROM lineitem AS l_sub
                                 WHERE l_sub.l_partkey = part.p_partkey);
