-- Scalar AS arrays are combined only after their independent shared scans.
WITH raw0 AS (
SELECT tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) promotions
   FROM main.store_sales,
        main.store,
        main.promotion,
        main.date_dim,
        main.customer,
        main.customer_address,
        main.item, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND  ss_sold_date_sk = d_date_sk
     AND ss_store_sk = s_store_sk
     AND ss_promo_sk = p_promo_sk
     AND ss_customer_sk= c_customer_sk
     AND ca_address_sk = c_current_addr_sk
     AND ss_item_sk = i_item_sk
     AND ca_gmt_offset = -5
     AND i_category = 'Jewelry'
     AND (p_channel_dmail = 'Y'
          OR p_channel_email = 'Y'
          OR p_channel_tv = 'Y')
     AND s_gmt_offset = -5
     AND d_year = 1998
     AND d_moy = 11
),
raw1 AS (
SELECT tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) total
   FROM main.store_sales,
        main.store,
        main.date_dim,
        main.customer,
        main.customer_address,
        main.item, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND  ss_sold_date_sk = d_date_sk
     AND ss_store_sk = s_store_sk
     AND ss_customer_sk= c_customer_sk
     AND ca_address_sk = c_current_addr_sk
     AND ss_item_sk = i_item_sk
     AND ca_gmt_offset = -5
     AND i_category = 'Jewelry'
     AND s_gmt_offset = -5
     AND d_year = 1998
     AND d_moy = 11
),
p AS (SELECT world, raw0.promotions[world+1] AS promotions, raw1.total[world+1] AS total FROM raw0 CROSS JOIN raw1 CROSS JOIN range(64) worlds(world))
SELECT world, p.promotions,
       p.total,
       cast(p.promotions AS decimal(15,4))/cast(p.total AS decimal(15,4))*100
 FROM p ORDER BY world;
