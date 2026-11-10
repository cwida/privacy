-- Each return channel is aggregated once, then joined on item and world.
WITH sr_items_stats AS (
SELECT i_item_id item_id,
          tpcds_sum(priv_hash(hash(sr_customer_sk)), sr_return_quantity) sr_item_qty,
 tpcds_count(priv_hash(hash(sr_customer_sk)),1) AS presence
FROM main.store_returns,
        main.item,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE sr_customer_sk=asa_owner.asa_key AND sr_item_sk = i_item_sk
     AND d_date IN
       (SELECT d_date
        FROM main.date_dim
        WHERE d_week_seq IN
            (SELECT d_week_seq
             FROM main.date_dim
             WHERE d_date IN ('2000-06-30',
                              '2000-09-27',
                              '2000-11-17')))
     AND sr_returned_date_sk = d_date_sk
   GROUP BY i_item_id
),
sr_items AS (
SELECT world, item_id, sr_item_qty[world+1] AS sr_item_qty
FROM sr_items_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
cr_items_stats AS (
SELECT i_item_id item_id,
          tpcds_sum(priv_hash(hash(cr_returning_customer_sk)), cr_return_quantity) cr_item_qty,
 tpcds_count(priv_hash(hash(cr_returning_customer_sk)),1) AS presence
FROM main.catalog_returns,
        main.item,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cr_returning_customer_sk=asa_owner.asa_key AND cr_item_sk = i_item_sk
     AND d_date IN
       (SELECT d_date
        FROM main.date_dim
        WHERE d_week_seq IN
            (SELECT d_week_seq
             FROM main.date_dim
             WHERE d_date IN ('2000-06-30',
                              '2000-09-27',
                              '2000-11-17')))
     AND cr_returned_date_sk = d_date_sk
   GROUP BY i_item_id
),
cr_items AS (
SELECT world, item_id, cr_item_qty[world+1] AS cr_item_qty
FROM cr_items_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
wr_items_stats AS (
SELECT i_item_id item_id,
          tpcds_sum(priv_hash(hash(wr_returning_customer_sk)), wr_return_quantity) wr_item_qty,
 tpcds_count(priv_hash(hash(wr_returning_customer_sk)),1) AS presence
FROM main.web_returns,
        main.item,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE wr_returning_customer_sk=asa_owner.asa_key AND wr_item_sk = i_item_sk
     AND d_date IN
       (SELECT d_date
        FROM main.date_dim
        WHERE d_week_seq IN
            (SELECT d_week_seq
             FROM main.date_dim
             WHERE d_date IN ('2000-06-30',
                              '2000-09-27',
                              '2000-11-17')))
     AND wr_returned_date_sk = d_date_sk
   GROUP BY i_item_id
),
wr_items AS (
SELECT world, item_id, wr_item_qty[world+1] AS wr_item_qty
FROM wr_items_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT sr_items.world AS world, sr_items.item_id ,
       sr_item_qty ,
       (sr_item_qty*1.0000)/(sr_item_qty+cr_item_qty+wr_item_qty)/3.0000 * 100 sr_dev ,
       cr_item_qty ,
       (cr_item_qty*1.0000)/(sr_item_qty+cr_item_qty+wr_item_qty)/3.0000 * 100 cr_dev ,
       wr_item_qty ,
       (wr_item_qty*1.0000)/(sr_item_qty+cr_item_qty+wr_item_qty)/3.0000 * 100 wr_dev ,
       (sr_item_qty+cr_item_qty+wr_item_qty)/3.0 average
FROM sr_items ,
     cr_items ,
     wr_items
WHERE sr_items.world=cr_items.world AND sr_items.world=wr_items.world AND  sr_items.item_id=cr_items.item_id
  AND sr_items.item_id=wr_items.item_id
QUALIFY row_number() OVER (PARTITION BY sr_items.world ORDER BY sr_items.item_id NULLS FIRST,
         sr_item_qty NULLS FIRST) <= 100
ORDER BY sr_items.world, sr_items.item_id NULLS FIRST,
         sr_item_qty NULLS FIRST;
