SELECT c_nation, s_nation, d_year, sum(lo_revenue) AS revenue
FROM customer, lineorder, supplier, date
WHERE lo_custkey = c_custkey AND lo_suppkey = s_suppkey AND lo_orderdate = d_datekey
  AND c_region = 'ASIA' AND s_region = 'ASIA' AND d_year BETWEEN 1992 AND 1997
GROUP BY c_nation, s_nation, d_year ORDER BY d_year, revenue DESC;
