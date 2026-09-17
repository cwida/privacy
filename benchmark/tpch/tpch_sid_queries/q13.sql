WITH c_orders AS (
    SELECT c.sample_id, c.c_custkey, count(o_orderkey) AS c_count
    FROM benchmark_customer_worlds c
    LEFT JOIN orders o ON c.c_custkey = o.o_custkey
      AND o_comment NOT LIKE '%special%requests%'
    GROUP BY c.sample_id, c.c_custkey
)
SELECT sample_id, c_count, count(*) AS custdist
FROM c_orders
GROUP BY sample_id, c_count
ORDER BY sample_id, custdist DESC, c_count DESC;
