SELECT
    100.00 * AVG(CASE
        WHEN p_type LIKE 'PROMO%'
        THEN 1.0
        ELSE 0.0
    END) AS promo_line_rate
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= CAST('1995-09-01' AS date)
  AND l_shipdate < CAST('1995-10-01' AS date);
