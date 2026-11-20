-- Matched and NULL-extended memberships are disjoint; NULL extension is evaluated separately in each world.
WITH left_input AS (
SELECT i_item_desc,w_warehouse_name,d1.d_week_seq,p_promo_sk,cs_order_number,cs_item_sk,priv_hash(hash(cs_bill_customer_sk)) AS membership
FROM main.catalog_sales JOIN main.customer ON cs_bill_customer_sk=c_customer_sk
JOIN main.inventory ON cs_item_sk=inv_item_sk JOIN main.warehouse ON w_warehouse_sk=inv_warehouse_sk
JOIN main.item ON i_item_sk=cs_item_sk JOIN main.customer_demographics ON cs_bill_cdemo_sk=cd_demo_sk
JOIN main.household_demographics ON cs_bill_hdemo_sk=hd_demo_sk
JOIN main.date_dim d1 ON cs_sold_date_sk=d1.d_date_sk JOIN main.date_dim d2 ON inv_date_sk=d2.d_date_sk JOIN main.date_dim d3 ON cs_ship_date_sk=d3.d_date_sk
LEFT JOIN main.promotion ON cs_promo_sk=p_promo_sk
WHERE d1.d_week_seq=d2.d_week_seq AND inv_quantity_on_hand<cs_quantity AND d3.d_date>d1.d_date+5
AND hd_buy_potential='>10000' AND d1.d_year=1999 AND cd_marital_status='D'
), valid_returns AS (SELECT r.*,priv_hash(hash(cr_returning_customer_sk)) AS membership FROM main.catalog_returns r JOIN main.customer ON cr_returning_customer_sk=c_customer_sk),
return_presence AS (SELECT cr_order_number,cr_item_sk,bit_or(membership) AS membership FROM valid_returns GROUP BY cr_order_number,cr_item_sk),
joined AS (
 SELECT l.i_item_desc,l.w_warehouse_name,l.d_week_seq,l.p_promo_sk,(l.membership & r.membership) AS membership FROM left_input l JOIN valid_returns r ON l.cs_order_number=r.cr_order_number AND l.cs_item_sk=r.cr_item_sk
 UNION ALL
 SELECT l.i_item_desc,l.w_warehouse_name,l.d_week_seq,l.p_promo_sk,(l.membership & ~coalesce(r.membership,0::UBIGINT)) AS membership FROM left_input l LEFT JOIN return_presence r ON l.cs_order_number=r.cr_order_number AND l.cs_item_sk=r.cr_item_sk
), stats AS (SELECT i_item_desc,w_warehouse_name,d_week_seq,tpcds_sum(membership,CASE WHEN p_promo_sk IS NULL THEN 1 ELSE 0 END) AS no_promo,tpcds_sum(membership,CASE WHEN p_promo_sk IS NOT NULL THEN 1 ELSE 0 END) AS promo,tpcds_count(membership,1) AS total_cnt,tpcds_count(membership,1) AS presence FROM joined GROUP BY i_item_desc,w_warehouse_name,d_week_seq),
answers AS (SELECT world,i_item_desc,w_warehouse_name,d_week_seq,no_promo[world+1] AS no_promo,promo[world+1] AS promo,total_cnt[world+1] AS total_cnt FROM stats CROSS JOIN range(64) worlds(world) WHERE presence[world+1]>0)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY total_cnt DESC NULLS FIRST,i_item_desc NULLS FIRST,w_warehouse_name NULLS FIRST,d_week_seq NULLS FIRST)<=100 ORDER BY world,total_cnt DESC NULLS FIRST,i_item_desc NULLS FIRST,w_warehouse_name NULLS FIRST,d_week_seq NULLS FIRST;
