-- Fact scans feed AS once; subsequent processing is over reduced aggregate results.
WITH ss_items_stats AS (
SELECT i_item_id item_id,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) ss_item_rev,
 tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales,
        main.item,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND ss_item_sk = i_item_sk
     AND d_date IN
       (SELECT d_date
        FROM main.date_dim
        WHERE d_week_seq =
            (SELECT d_week_seq
             FROM main.date_dim
             WHERE d_date = '2000-01-03'))
     AND ss_sold_date_sk = d_date_sk
   GROUP BY i_item_id
),
ss_items AS (
SELECT world, item_id, ss_item_rev[world+1] AS ss_item_rev
FROM ss_items_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
cs_items_stats AS (
SELECT i_item_id item_id,
          tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), cs_ext_sales_price) cs_item_rev,
 tpcds_count(priv_hash(hash(cs_bill_customer_sk)),1) AS presence
FROM main.catalog_sales,
        main.item,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cs_bill_customer_sk=asa_owner.asa_key AND cs_item_sk = i_item_sk
     AND d_date IN
       (SELECT d_date
        FROM main.date_dim
        WHERE d_week_seq =
            (SELECT d_week_seq
             FROM main.date_dim
             WHERE d_date = '2000-01-03'))
     AND cs_sold_date_sk = d_date_sk
   GROUP BY i_item_id
),
cs_items AS (
SELECT world, item_id, cs_item_rev[world+1] AS cs_item_rev
FROM cs_items_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
ws_items_stats AS (
SELECT i_item_id item_id,
          tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), ws_ext_sales_price) ws_item_rev,
 tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) AS presence
FROM main.web_sales,
        main.item,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND ws_item_sk = i_item_sk
     AND d_date IN
       (SELECT d_date
        FROM main.date_dim
        WHERE d_week_seq =
            (SELECT d_week_seq
             FROM main.date_dim
             WHERE d_date = '2000-01-03'))
     AND ws_sold_date_sk = d_date_sk
   GROUP BY i_item_id
),
ws_items AS (
SELECT world, item_id, ws_item_rev[world+1] AS ws_item_rev
FROM ws_items_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT ss_items.world AS world, ss_items.item_id,
       ss_item_rev,
       ss_item_rev/((ss_item_rev+cs_item_rev+ws_item_rev)/3) * 100 ss_dev,
       cs_item_rev,
       cs_item_rev/((ss_item_rev+cs_item_rev+ws_item_rev)/3) * 100 cs_dev,
       ws_item_rev,
       ws_item_rev/((ss_item_rev+cs_item_rev+ws_item_rev)/3) * 100 ws_dev,
       (ss_item_rev+cs_item_rev+ws_item_rev)/3 average
FROM ss_items,
     cs_items,
     ws_items
WHERE ss_items.world=cs_items.world AND ss_items.world=ws_items.world AND  ss_items.item_id=cs_items.item_id
  AND ss_items.item_id=ws_items.item_id
  AND ss_item_rev BETWEEN 0.9 * cs_item_rev AND 1.1 * cs_item_rev
  AND ss_item_rev BETWEEN 0.9 * ws_item_rev AND 1.1 * ws_item_rev
  AND cs_item_rev BETWEEN 0.9 * ss_item_rev AND 1.1 * ss_item_rev
  AND cs_item_rev BETWEEN 0.9 * ws_item_rev AND 1.1 * ws_item_rev
  AND ws_item_rev BETWEEN 0.9 * ss_item_rev AND 1.1 * ss_item_rev
  AND ws_item_rev BETWEEN 0.9 * cs_item_rev AND 1.1 * cs_item_rev
QUALIFY row_number() OVER (PARTITION BY ss_items.world ORDER BY ss_items.item_id NULLS FIRST,
         ss_item_rev NULLS FIRST) <= 100
ORDER BY ss_items.world, ss_items.item_id NULLS FIRST,
         ss_item_rev NULLS FIRST;
