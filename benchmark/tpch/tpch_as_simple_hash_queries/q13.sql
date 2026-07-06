SELECT c_count, as_noised_count(priv_hash(hash(c_custkey))) AS custdist
  FROM (SELECT c_custkey, count(o_orderkey)
          FROM customer LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE '%special%requests%'
         GROUP BY ALL) AS c_orders (c_custkey, c_count)
 GROUP BY ALL
 ORDER BY custdist DESC, c_count DESC;
