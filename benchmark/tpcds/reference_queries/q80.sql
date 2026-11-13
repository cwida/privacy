-- Matched/fallback row masks preserve sampled outer joins before each channel's AS aggregates.
WITH ssr_left AS (
SELECT s_store_id AS store_id,ss_ticket_number AS order_number,ss_item_sk AS item_sk,ss_ext_sales_price AS sales_price,ss_net_profit AS net_profit,priv_hash(hash(ss_customer_sk)) AS membership
FROM main.store_sales JOIN main.customer ON ss_customer_sk=c_customer_sk JOIN main.date_dim ON ss_sold_date_sk=d_date_sk
JOIN main.store ON ss_store_sk=s_store_sk JOIN main.item ON ss_item_sk=i_item_sk JOIN main.promotion ON ss_promo_sk=p_promo_sk
WHERE d_date BETWEEN DATE '2000-08-23' AND DATE '2000-09-22' AND i_current_price>50 AND p_channel_tv='N'
),
ssr_returns AS (SELECT sr_ticket_number AS order_number,sr_item_sk AS item_sk,sr_return_amt AS amount,sr_net_loss AS loss,priv_hash(hash(sr_customer_sk)) AS membership FROM main.store_returns JOIN main.customer ON sr_customer_sk=c_customer_sk),
ssr_return_presence AS (SELECT order_number,item_sk,bit_or(membership) AS membership FROM ssr_returns GROUP BY order_number,item_sk),
ssr_joined AS (
SELECT l.store_id,l.sales_price,l.net_profit,r.amount,r.loss,(l.membership & r.membership) AS membership FROM ssr_left l JOIN ssr_returns r ON l.order_number=r.order_number AND l.item_sk=r.item_sk
UNION ALL SELECT l.store_id,l.sales_price,l.net_profit,0 AS amount,0 AS loss,(l.membership & ~coalesce(r.membership,0::UBIGINT)) AS membership FROM ssr_left l LEFT JOIN ssr_return_presence r ON l.order_number=r.order_number AND l.item_sk=r.item_sk
),
ssr_stats AS (SELECT store_id,tpcds_sum(membership,sales_price) AS sales,tpcds_sum(membership,coalesce(amount,0)) AS returns_,tpcds_sum(membership,net_profit-coalesce(loss,0)) AS profit,tpcds_count(membership,1) AS presence FROM ssr_joined GROUP BY store_id),
ssr AS (
SELECT world, store_id, sales[world+1] AS sales, returns_[world+1] AS returns_, profit[world+1] AS profit
FROM ssr_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
csr_left AS (
SELECT cp_catalog_page_id AS catalog_page_id,cs_order_number AS order_number,cs_item_sk AS item_sk,cs_ext_sales_price AS sales_price,cs_net_profit AS net_profit,priv_hash(hash(cs_bill_customer_sk)) AS membership
FROM main.catalog_sales JOIN main.customer ON cs_bill_customer_sk=c_customer_sk JOIN main.date_dim ON cs_sold_date_sk=d_date_sk
JOIN main.catalog_page ON cs_catalog_page_sk=cp_catalog_page_sk JOIN main.item ON cs_item_sk=i_item_sk JOIN main.promotion ON cs_promo_sk=p_promo_sk
WHERE d_date BETWEEN DATE '2000-08-23' AND DATE '2000-09-22' AND i_current_price>50 AND p_channel_tv='N'
),
csr_returns AS (SELECT cr_order_number AS order_number,cr_item_sk AS item_sk,cr_return_amount AS amount,cr_net_loss AS loss,priv_hash(hash(cr_returning_customer_sk)) AS membership FROM main.catalog_returns JOIN main.customer ON cr_returning_customer_sk=c_customer_sk),
csr_return_presence AS (SELECT order_number,item_sk,bit_or(membership) AS membership FROM csr_returns GROUP BY order_number,item_sk),
csr_joined AS (
SELECT l.catalog_page_id,l.sales_price,l.net_profit,r.amount,r.loss,(l.membership & r.membership) AS membership FROM csr_left l JOIN csr_returns r ON l.order_number=r.order_number AND l.item_sk=r.item_sk
UNION ALL SELECT l.catalog_page_id,l.sales_price,l.net_profit,0 AS amount,0 AS loss,(l.membership & ~coalesce(r.membership,0::UBIGINT)) AS membership FROM csr_left l LEFT JOIN csr_return_presence r ON l.order_number=r.order_number AND l.item_sk=r.item_sk
),
csr_stats AS (SELECT catalog_page_id,tpcds_sum(membership,sales_price) AS sales,tpcds_sum(membership,coalesce(amount,0)) AS returns_,tpcds_sum(membership,net_profit-coalesce(loss,0)) AS profit,tpcds_count(membership,1) AS presence FROM csr_joined GROUP BY catalog_page_id),
csr AS (
SELECT world, catalog_page_id, sales[world+1] AS sales, returns_[world+1] AS returns_, profit[world+1] AS profit
FROM csr_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
wsr_left AS (
SELECT web_site_id AS web_site_id,ws_order_number AS order_number,ws_item_sk AS item_sk,ws_ext_sales_price AS sales_price,ws_net_profit AS net_profit,priv_hash(hash(ws_bill_customer_sk)) AS membership
FROM main.web_sales JOIN main.customer ON ws_bill_customer_sk=c_customer_sk JOIN main.date_dim ON ws_sold_date_sk=d_date_sk
JOIN main.web_site ON ws_web_site_sk=web_site_sk JOIN main.item ON ws_item_sk=i_item_sk JOIN main.promotion ON ws_promo_sk=p_promo_sk
WHERE d_date BETWEEN DATE '2000-08-23' AND DATE '2000-09-22' AND i_current_price>50 AND p_channel_tv='N'
),
wsr_returns AS (SELECT wr_order_number AS order_number,wr_item_sk AS item_sk,wr_return_amt AS amount,wr_net_loss AS loss,priv_hash(hash(wr_returning_customer_sk)) AS membership FROM main.web_returns JOIN main.customer ON wr_returning_customer_sk=c_customer_sk),
wsr_return_presence AS (SELECT order_number,item_sk,bit_or(membership) AS membership FROM wsr_returns GROUP BY order_number,item_sk),
wsr_joined AS (
SELECT l.web_site_id,l.sales_price,l.net_profit,r.amount,r.loss,(l.membership & r.membership) AS membership FROM wsr_left l JOIN wsr_returns r ON l.order_number=r.order_number AND l.item_sk=r.item_sk
UNION ALL SELECT l.web_site_id,l.sales_price,l.net_profit,0 AS amount,0 AS loss,(l.membership & ~coalesce(r.membership,0::UBIGINT)) AS membership FROM wsr_left l LEFT JOIN wsr_return_presence r ON l.order_number=r.order_number AND l.item_sk=r.item_sk
),
wsr_stats AS (SELECT web_site_id,tpcds_sum(membership,sales_price) AS sales,tpcds_sum(membership,coalesce(amount,0)) AS returns_,tpcds_sum(membership,net_profit-coalesce(loss,0)) AS profit,tpcds_count(membership,1) AS presence FROM wsr_joined GROUP BY web_site_id),
wsr AS (
SELECT world, web_site_id, sales[world+1] AS sales, returns_[world+1] AS returns_, profit[world+1] AS profit
FROM wsr_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
channels AS (SELECT world,'store channel' AS channel,concat('store',store_id) AS id,sales,returns_,profit FROM ssr UNION ALL SELECT world,'catalog channel',concat('catalog_page',catalog_page_id),sales,returns_,profit FROM csr UNION ALL SELECT world,'web channel',concat('web_site',web_site_id),sales,returns_,profit FROM wsr),
answers AS (SELECT w.world,g.* FROM range(64) w(world) CROSS JOIN LATERAL (SELECT channel,id,sum(sales) AS sales,sum(returns_) AS returns_,sum(profit) AS profit FROM channels WHERE channels.world=w.world GROUP BY ROLLUP(channel,id)) g)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY channel NULLS FIRST,id NULLS FIRST)<=100 ORDER BY world,channel NULLS FIRST,id NULLS FIRST;
