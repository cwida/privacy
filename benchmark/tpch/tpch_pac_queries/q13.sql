SELECT c_count, as_noised_count(priv_hash(hash(c_custkey))) AS custdist
  FROM (SELECT customer.c_custkey, count(o_orderkey) AS c_count
          FROM customer LEFT OUTER JOIN orders ON customer.c_custkey = orders.o_custkey
                                              AND orders.o_comment NOT LIKE '%special%requests%'
         GROUP BY customer.c_custkey) AS c_orders (c_custkey, c_count)
 GROUP BY ALL
 ORDER BY custdist DESC, c_count DESC;
