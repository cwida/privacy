SELECT as_sum(priv_hash(hash(lo_custkey)), lo_extendedprice * lo_discount) AS revenue,
       as_count(priv_hash(hash(lo_custkey))) AS ssb_presence
FROM lineorder, date
WHERE lo_orderdate = d_datekey AND d_yearmonthnum = 199401
  AND lo_discount BETWEEN 4 AND 6 AND lo_quantity BETWEEN 26 AND 35;
