-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- Ratio arithmetic is lane-wise; only final outputs are scalarized, matching categorical compilation.
SELECT CAST(as_noised(promotions) AS DECIMAL(38,2)) AS promotions, CAST(as_noised(total) AS DECIMAL(38,2)) AS total, CAST(as_noised(list_transform(list_zip(promotions,total), lambda x: CAST(x[1] AS DECIMAL(15,4)) / CAST(x[2] AS DECIMAL(15,4)) * 100)::FLOAT[]) AS DOUBLE)
FROM (SELECT as_sum(priv_hash(hash(c_customer_sk)), ss_ext_sales_price) promotions
   FROM store_sales,
        store,
        promotion,
        date_dim,
        customer,
        customer_address,
        item
   WHERE ss_sold_date_sk = d_date_sk
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
     AND d_moy = 11) promotional_sales, (SELECT as_sum(priv_hash(hash(c_customer_sk)), ss_ext_sales_price) total
   FROM store_sales,
        store,
        date_dim,
        customer,
        customer_address,
        item
   WHERE ss_sold_date_sk = d_date_sk
     AND ss_store_sk = s_store_sk
     AND ss_customer_sk= c_customer_sk
     AND ca_address_sk = c_current_addr_sk
     AND ss_item_sk = i_item_sk
     AND ca_gmt_offset = -5
     AND i_category = 'Jewelry'
     AND s_gmt_offset = -5
     AND d_year = 1998
     AND d_moy = 11)  all_sales
ORDER BY promotions,
         total
LIMIT 100;
