-- q17: 64-possible-worlds-semantics with lambda expressions.
-- the priv_filter makes a probabilistic choice over 64 booleans
-- the outer list_transform-lambda computes the expression, for all 64 possible worlds
--  the inner list_transform just casts. This is needed to make the original expression (l_quantity < 0.2 * x) work safely
SELECT as_noised_sum(priv_hash(hash(o_custkey)), l_extendedprice) / 7.0 AS avg_yearly
  FROM lineitem JOIN part ON lineitem.l_partkey = part.p_partkey JOIN orders ON lineitem.l_orderkey = orders.o_orderkey
 WHERE part.p_brand = 'Brand#23' AND part.p_container = 'MED BOX' AND
       priv_filter(
         list_transform(
           list_transform(
             (SELECT list_transform(
                       list_zip(as_sum(priv_hash(hash(o_sub.o_custkey)), l_sub.l_quantity),
                                as_count(priv_hash(hash(o_sub.o_custkey)), l_sub.l_quantity)),
                       lambda x: x[1] / x[2])
                FROM lineitem AS l_sub JOIN orders AS o_sub ON l_sub.l_orderkey = o_sub.o_orderkey
               WHERE l_sub.l_partkey = part.p_partkey),
            lambda y: CAST(y AS DECIMAL(18,2))),
          lambda x: lineitem.l_quantity < 0.2 * x));
