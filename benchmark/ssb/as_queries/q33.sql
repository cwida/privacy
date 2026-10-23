SELECT c_city, s_city, d_year, as_sum(priv_hash(hash(lo_custkey)), lo_revenue) AS revenue,
       as_count(priv_hash(hash(lo_custkey))) AS ssb_presence
FROM customer, lineorder, supplier, date
WHERE lo_custkey = c_custkey AND lo_suppkey = s_suppkey AND lo_orderdate = d_datekey
  AND c_city IN ('UNITED KI1','UNITED KI5') AND s_city IN ('UNITED KI1','UNITED KI5')
  AND d_year BETWEEN 1992 AND 1997
GROUP BY c_city, s_city, d_year ORDER BY d_year, revenue DESC;
