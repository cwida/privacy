SELECT c_nation, s_nation, d_year, as_sum(priv_hash(hash(lo_custkey)), lo_revenue) AS revenue,
       as_count(priv_hash(hash(lo_custkey))) AS ssb_presence
FROM customer, lineorder, supplier, date
WHERE lo_custkey = c_custkey AND lo_suppkey = s_suppkey AND lo_orderdate = d_datekey
  AND c_region = 'ASIA' AND s_region = 'ASIA' AND d_year BETWEEN 1992 AND 1997
GROUP BY c_nation, s_nation, d_year ORDER BY d_year, revenue DESC;
