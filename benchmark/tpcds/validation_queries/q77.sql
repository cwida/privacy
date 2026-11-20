-- Grouped channel results retain world-local LEFT JOIN semantics and empty-world ROLLUP totals.
WITH ss_stats AS (
SELECT s_store_sk,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) AS sales,
          tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_net_profit) AS profit,
 tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales,
        main.date_dim,
        main.store, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND ss_sold_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND ss_store_sk = s_store_sk
   GROUP BY s_store_sk
),
ss AS (
SELECT world, s_store_sk, sales[world+1] AS sales, profit[world+1] AS profit
FROM ss_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
sr_stats AS (
SELECT s_store_sk,
          tpcds_sum(priv_hash(hash(sr_customer_sk)), sr_return_amt) AS returns_,
          tpcds_sum(priv_hash(hash(sr_customer_sk)), sr_net_loss) AS profit_loss,
 tpcds_count(priv_hash(hash(sr_customer_sk)),1) AS presence
FROM main.store_returns,
        main.date_dim,
        main.store, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE sr_customer_sk=asa_owner.asa_key AND sr_returned_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND sr_store_sk = s_store_sk
   GROUP BY s_store_sk
),
sr AS (
SELECT world, s_store_sk, returns_[world+1] AS returns_, profit_loss[world+1] AS profit_loss
FROM sr_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
cs_stats AS (
SELECT cs_call_center_sk,
          tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), cs_ext_sales_price) AS sales,
          tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), cs_net_profit) AS profit,
 tpcds_count(priv_hash(hash(cs_bill_customer_sk)),1) AS presence
FROM main.catalog_sales,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cs_bill_customer_sk=asa_owner.asa_key AND cs_sold_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
   GROUP BY cs_call_center_sk
),
cs AS (
SELECT world, cs_call_center_sk, sales[world+1] AS sales, profit[world+1] AS profit
FROM cs_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
cr_stats AS (
SELECT cr_call_center_sk,
          tpcds_sum(priv_hash(hash(cr_returning_customer_sk)), cr_return_amount) AS returns_,
          tpcds_sum(priv_hash(hash(cr_returning_customer_sk)), cr_net_loss) AS profit_loss,
 tpcds_count(priv_hash(hash(cr_returning_customer_sk)),1) AS presence
FROM main.catalog_returns,
        main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cr_returning_customer_sk=asa_owner.asa_key AND cr_returned_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
   GROUP BY cr_call_center_sk
),
cr AS (
SELECT world, cr_call_center_sk, returns_[world+1] AS returns_, profit_loss[world+1] AS profit_loss
FROM cr_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
ws_stats AS (
SELECT wp_web_page_sk,
          tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), ws_ext_sales_price) AS sales,
          tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), ws_net_profit) AS profit,
 tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) AS presence
FROM main.web_sales,
        main.date_dim,
        main.web_page, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND ws_sold_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND ws_web_page_sk = wp_web_page_sk
   GROUP BY wp_web_page_sk
),
ws AS (
SELECT world, wp_web_page_sk, sales[world+1] AS sales, profit[world+1] AS profit
FROM ws_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
wr_stats AS (
SELECT wp_web_page_sk,
          tpcds_sum(priv_hash(hash(wr_returning_customer_sk)), wr_return_amt) AS returns_,
          tpcds_sum(priv_hash(hash(wr_returning_customer_sk)), wr_net_loss) AS profit_loss,
 tpcds_count(priv_hash(hash(wr_returning_customer_sk)),1) AS presence
FROM main.web_returns,
        main.date_dim,
        main.web_page, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE wr_returning_customer_sk=asa_owner.asa_key AND wr_returned_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND wr_web_page_sk = wp_web_page_sk
   GROUP BY wp_web_page_sk
),
wr AS (
SELECT world, wp_web_page_sk, returns_[world+1] AS returns_, profit_loss[world+1] AS profit_loss
FROM wr_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
channels AS (
SELECT ss.world,'store channel' AS channel,ss.s_store_sk AS id,sales,coalesce(returns_,0) AS returns_,profit-coalesce(profit_loss,0) AS profit FROM ss LEFT JOIN sr ON ss.world=sr.world AND ss.s_store_sk=sr.s_store_sk
UNION ALL SELECT cs.world,'catalog channel',cs_call_center_sk,sales,returns_,profit-profit_loss FROM cs JOIN cr ON cs.world=cr.world
UNION ALL SELECT ws.world,'web channel',ws.wp_web_page_sk,sales,coalesce(returns_,0),profit-coalesce(profit_loss,0) FROM ws LEFT JOIN wr ON ws.world=wr.world AND ws.wp_web_page_sk=wr.wp_web_page_sk
),
answers AS (
SELECT w.world,g.* FROM range(64) w(world) CROSS JOIN LATERAL (SELECT channel,id,sum(sales) AS sales,sum(returns_) AS returns_,sum(profit) AS profit FROM channels WHERE channels.world=w.world GROUP BY ROLLUP(channel,id)) g
)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY channel NULLS FIRST,id NULLS FIRST,returns_ DESC)<=100 ORDER BY world,channel NULLS FIRST,id NULLS FIRST,returns_ DESC;
