SELECT 100.0 * as_noised_sum(priv_hash(hash(l_orderkey)), CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)
             / as_noised_sum(priv_hash(hash(l_orderkey)), l_extendedprice * (1 - l_discount)) AS promo_revenue
  FROM lineitem JOIN part ON l_partkey = p_partkey
 WHERE l_shipdate >= DATE '1995-09-01' AND l_shipdate < DATE '1995-10-01';
