SELECT c_city, s_city, d_year, sum(lo_revenue) AS revenue
FROM customer, lineorder, supplier, date
WHERE lo_custkey = c_custkey AND lo_suppkey = s_suppkey AND lo_orderdate = d_datekey
  AND c_nation = 'UNITED STATES' AND s_nation = 'UNITED STATES' AND d_year BETWEEN 1992 AND 1997
GROUP BY c_city, s_city, d_year ORDER BY d_year, revenue DESC;
