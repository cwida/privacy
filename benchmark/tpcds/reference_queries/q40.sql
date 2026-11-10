-- Matched and NULL-extended memberships are disjoint; NULL extension is evaluated separately in each world.
WITH left_input AS (
SELECT w_state,i_item_id,d_date,cs_sales_price,cs_order_number,cs_item_sk,priv_hash(hash(cs_bill_customer_sk)) AS membership
FROM main.catalog_sales JOIN main.customer ON cs_bill_customer_sk=c_customer_sk
JOIN main.warehouse ON cs_warehouse_sk=w_warehouse_sk JOIN main.item ON i_item_sk=cs_item_sk
JOIN main.date_dim ON cs_sold_date_sk=d_date_sk
WHERE i_current_price BETWEEN 0.99 AND 1.49 AND d_date BETWEEN DATE '2000-02-10' AND DATE '2000-04-10' 
), valid_returns AS (SELECT r.*,priv_hash(hash(cr_returning_customer_sk)) AS membership FROM main.catalog_returns r JOIN main.customer ON cr_returning_customer_sk=c_customer_sk),
return_presence AS (SELECT cr_order_number,cr_item_sk,bit_or(membership) AS membership FROM valid_returns GROUP BY cr_order_number,cr_item_sk),
joined AS (
 SELECT l.w_state,l.i_item_id,l.d_date,l.cs_sales_price,coalesce(r.cr_refunded_cash,0) AS refund,(l.membership & r.membership) AS membership FROM left_input l JOIN valid_returns r ON l.cs_order_number=r.cr_order_number AND l.cs_item_sk=r.cr_item_sk
 UNION ALL
 SELECT l.w_state,l.i_item_id,l.d_date,l.cs_sales_price,0 AS refund,(l.membership & ~coalesce(r.membership,0::UBIGINT)) AS membership FROM left_input l LEFT JOIN return_presence r ON l.cs_order_number=r.cr_order_number AND l.cs_item_sk=r.cr_item_sk
), stats AS (SELECT w_state,i_item_id,tpcds_sum(membership,CASE WHEN d_date<DATE '2000-03-11' THEN cs_sales_price-refund ELSE 0 END) AS sales_before,tpcds_sum(membership,CASE WHEN d_date>=DATE '2000-03-11' THEN cs_sales_price-refund ELSE 0 END) AS sales_after,tpcds_count(membership,1) AS presence FROM joined GROUP BY w_state,i_item_id),
answers AS (SELECT world,w_state,i_item_id,sales_before[world+1] AS sales_before,sales_after[world+1] AS sales_after FROM stats CROSS JOIN range(64) worlds(world) WHERE presence[world+1]>0)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY w_state,i_item_id)<=100 ORDER BY world,w_state,i_item_id;
