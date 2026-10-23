SELECT d_year, s_nation, p_category, as_sum(priv_hash(hash(lo_custkey)), lo_revenue - lo_supplycost) AS profit,
       as_count(priv_hash(hash(lo_custkey))) AS ssb_presence
FROM date, customer, supplier, part, lineorder
WHERE lo_custkey = c_custkey AND lo_suppkey = s_suppkey AND lo_partkey = p_partkey
  AND lo_orderdate = d_datekey AND c_region = 'AMERICA' AND s_region = 'AMERICA'
  AND d_year IN (1997,1998) AND p_mfgr IN ('MFGR#1','MFGR#2')
GROUP BY d_year, s_nation, p_category ORDER BY d_year, s_nation, p_category;
