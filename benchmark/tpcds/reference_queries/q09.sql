-- Each CASE selects between its world-specific aggregate results.
WITH stats AS (SELECT tpcds_count(CASE WHEN ss_quantity BETWEEN 1 AND 20 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS n1,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 1 AND 20 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_ext_discount_amt) AS discount1,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 1 AND 20 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_net_paid) AS paid1,
tpcds_count(CASE WHEN ss_quantity BETWEEN 21 AND 40 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS n2,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 21 AND 40 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_ext_discount_amt) AS discount2,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 21 AND 40 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_net_paid) AS paid2,
tpcds_count(CASE WHEN ss_quantity BETWEEN 41 AND 60 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS n3,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 41 AND 60 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_ext_discount_amt) AS discount3,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 41 AND 60 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_net_paid) AS paid3,
tpcds_count(CASE WHEN ss_quantity BETWEEN 61 AND 80 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS n4,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 61 AND 80 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_ext_discount_amt) AS discount4,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 61 AND 80 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_net_paid) AS paid4,
tpcds_count(CASE WHEN ss_quantity BETWEEN 81 AND 100 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,1) AS n5,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 81 AND 100 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_ext_discount_amt) AS discount5,
tpcds_avg(CASE WHEN ss_quantity BETWEEN 81 AND 100 THEN priv_hash(hash(ss_customer_sk)) ELSE 0::UBIGINT END,ss_net_paid) AS paid5 FROM main.store_sales JOIN main.customer ON ss_customer_sk=c_customer_sk)
SELECT world, CASE WHEN n1[world+1]>74129 THEN discount1[world+1] ELSE paid1[world+1] END AS bucket1, CASE WHEN n2[world+1]>122840 THEN discount2[world+1] ELSE paid2[world+1] END AS bucket2, CASE WHEN n3[world+1]>56580 THEN discount3[world+1] ELSE paid3[world+1] END AS bucket3, CASE WHEN n4[world+1]>10097 THEN discount4[world+1] ELSE paid4[world+1] END AS bucket4, CASE WHEN n5[world+1]>165306 THEN discount5[world+1] ELSE paid5[world+1] END AS bucket5 FROM stats CROSS JOIN range(64) worlds(world) CROSS JOIN main.reason WHERE r_reason_sk=1 ORDER BY world;
