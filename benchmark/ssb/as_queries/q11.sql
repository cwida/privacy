SELECT as_sum(priv_hash(hash(lo_custkey)), lo_extendedprice * lo_discount) AS revenue,
       as_count(priv_hash(hash(lo_custkey))) AS ssb_presence
FROM lineorder, date
WHERE lo_orderdate = d_datekey AND d_year = 1993
  AND lo_discount BETWEEN 1 AND 3 AND lo_quantity < 25;
