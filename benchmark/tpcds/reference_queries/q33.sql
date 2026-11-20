-- Fact scans feed AS once; subsequent processing is over reduced aggregate results.
WITH ss_stats AS (
SELECT i_manufact_id,
           tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) total_sales,
 tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales,
        main.date_dim,
        main.customer_address,
        main.item, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND i_manufact_id IN
       (SELECT i_manufact_id
        FROM main.item
        WHERE i_category IN ('Electronics'))
     AND ss_item_sk = i_item_sk
     AND ss_sold_date_sk = d_date_sk
     AND d_year = 1998
     AND d_moy = 5
     AND ss_addr_sk = ca_address_sk
     AND ca_gmt_offset = -5
   GROUP BY i_manufact_id
),
ss AS (
SELECT world, i_manufact_id, total_sales[world+1] AS total_sales
FROM ss_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
cs_stats AS (
SELECT i_manufact_id,
           tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), cs_ext_sales_price) total_sales,
 tpcds_count(priv_hash(hash(cs_bill_customer_sk)),1) AS presence
FROM main.catalog_sales,
        main.date_dim,
        main.customer_address,
        main.item, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cs_bill_customer_sk=asa_owner.asa_key AND i_manufact_id IN
       (SELECT i_manufact_id
        FROM main.item
        WHERE i_category IN ('Electronics'))
     AND cs_item_sk = i_item_sk
     AND cs_sold_date_sk = d_date_sk
     AND d_year = 1998
     AND d_moy = 5
     AND cs_bill_addr_sk = ca_address_sk
     AND ca_gmt_offset = -5
   GROUP BY i_manufact_id
),
cs AS (
SELECT world, i_manufact_id, total_sales[world+1] AS total_sales
FROM cs_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
ws_stats AS (
SELECT i_manufact_id,
           tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), ws_ext_sales_price) total_sales,
 tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) AS presence
FROM main.web_sales,
        main.date_dim,
        main.customer_address,
        main.item, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND i_manufact_id IN
       (SELECT i_manufact_id
        FROM main.item
        WHERE i_category IN ('Electronics'))
     AND ws_item_sk = i_item_sk
     AND ws_sold_date_sk = d_date_sk
     AND d_year = 1998
     AND d_moy = 5
     AND ws_bill_addr_sk = ca_address_sk
     AND ca_gmt_offset = -5
   GROUP BY i_manufact_id
),
ws AS (
SELECT world, i_manufact_id, total_sales[world+1] AS total_sales
FROM ws_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
combined AS (
SELECT world, i_manufact_id,
       sum(total_sales) total_sales
FROM
  (SELECT *
   FROM ss
   UNION ALL SELECT *
   FROM cs
   UNION ALL SELECT *
   FROM ws) tmp1
GROUP BY world, i_manufact_id
)
SELECT * FROM combined QUALIFY row_number() OVER (PARTITION BY world ORDER BY total_sales) <= 100
ORDER BY world, total_sales;
