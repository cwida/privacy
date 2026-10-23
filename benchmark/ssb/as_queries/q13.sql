SELECT as_sum(priv_hash(hash(lo_custkey)), lo_extendedprice * lo_discount) AS revenue,
       as_count(priv_hash(hash(lo_custkey))) AS ssb_presence
FROM lineorder, date
WHERE lo_orderdate = d_datekey AND d_weeknuminyear = 6 AND d_year = 1994
  AND lo_discount BETWEEN 5 AND 7 AND lo_quantity BETWEEN 26 AND 35;
