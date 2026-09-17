WITH revenue AS (
    SELECT c.sample_id, l_suppkey AS supplier_no,
           sum(l_extendedprice * (1 - l_discount)) AS total_revenue
    FROM lineitem l
    JOIN orders o ON l.l_orderkey = o.o_orderkey
    JOIN benchmark_customer_worlds c ON o.o_custkey = c.c_custkey
    WHERE l_shipdate >= DATE '1996-01-01'
      AND l_shipdate < DATE '1996-04-01'
    GROUP BY c.sample_id, l_suppkey
), maxima AS (
    SELECT sample_id, max(total_revenue) AS max_revenue
    FROM revenue GROUP BY sample_id
)
SELECT r.sample_id, s_suppkey, s_name, s_address, s_phone, total_revenue
FROM revenue r
JOIN maxima m ON r.sample_id = m.sample_id AND r.total_revenue = m.max_revenue
JOIN supplier s ON s.s_suppkey = r.supplier_no
ORDER BY r.sample_id, s_suppkey;
