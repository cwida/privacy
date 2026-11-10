-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- Keep all six original branches, including the original catalog Cartesian product.
WITH ss AS (SELECT s_store_sk,
          as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) AS sales,
          as_noised_sum(priv_hash(hash(ss_customer_sk)), ss_net_profit) AS profit
   FROM store_sales,
        date_dim,
        store
   WHERE ss_sold_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND ss_store_sk = s_store_sk
   GROUP BY s_store_sk),
sr AS (SELECT s_store_sk,
          as_noised_sum(priv_hash(hash(sr_customer_sk)), sr_return_amt) AS returns_,
          as_noised_sum(priv_hash(hash(sr_customer_sk)), sr_net_loss) AS profit_loss
   FROM store_returns,
        date_dim,
        store
   WHERE sr_returned_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND sr_store_sk = s_store_sk
   GROUP BY s_store_sk),
cs AS (SELECT cs_call_center_sk,
          as_noised_sum(priv_hash(hash(cs_bill_customer_sk)), cs_ext_sales_price) AS sales,
          as_noised_sum(priv_hash(hash(cs_bill_customer_sk)), cs_net_profit) AS profit
   FROM catalog_sales,
        date_dim
   WHERE cs_sold_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
   GROUP BY cs_call_center_sk),
cr AS (SELECT cr_call_center_sk,
          as_noised_sum(priv_hash(hash(cr_returning_customer_sk)), cr_return_amount) AS returns_,
          as_noised_sum(priv_hash(hash(cr_returning_customer_sk)), cr_net_loss) AS profit_loss
   FROM catalog_returns,
        date_dim
   WHERE cr_returned_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
   GROUP BY cr_call_center_sk ),
ws AS (SELECT wp_web_page_sk,
          as_noised_sum(priv_hash(hash(ws_bill_customer_sk)), ws_ext_sales_price) AS sales,
          as_noised_sum(priv_hash(hash(ws_bill_customer_sk)), ws_net_profit) AS profit
   FROM web_sales,
        date_dim,
        web_page
   WHERE ws_sold_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND ws_web_page_sk = wp_web_page_sk
   GROUP BY wp_web_page_sk),
wr AS (SELECT wp_web_page_sk,
          as_noised_sum(priv_hash(hash(wr_returning_customer_sk)), wr_return_amt) AS returns_,
          as_noised_sum(priv_hash(hash(wr_returning_customer_sk)), wr_net_loss) AS profit_loss
   FROM web_returns,
        date_dim,
        web_page
   WHERE wr_returned_date_sk = d_date_sk
     AND d_date BETWEEN cast('2000-08-23' AS date) AND cast('2000-09-22' AS date)
     AND wr_web_page_sk = wp_web_page_sk
   GROUP BY wp_web_page_sk)
SELECT channel ,
       id ,
       sum(sales) AS sales ,
       sum(returns_) AS returns_ ,
       sum(profit) AS profit
FROM
  (SELECT 'store channel' AS channel ,
          ss.s_store_sk AS id ,
          sales ,
          coalesce(returns_, 0) AS returns_ ,
          (profit - coalesce(profit_loss,0)) AS profit
   FROM ss
   LEFT JOIN sr ON ss.s_store_sk = sr.s_store_sk
   UNION ALL SELECT 'catalog channel' AS channel ,
                    cs_call_center_sk AS id ,
                    sales ,
                    returns_ ,
                    (profit - profit_loss) AS profit
   FROM cs ,
        cr
   UNION ALL SELECT 'web channel' AS channel ,
                    ws.wp_web_page_sk AS id ,
                    sales ,
                    coalesce(returns_, 0) returns_ ,
                    (profit - coalesce(profit_loss,0)) AS profit
   FROM ws
   LEFT JOIN wr ON ws.wp_web_page_sk = wr.wp_web_page_sk ) x
GROUP BY ROLLUP (channel,
                 id)
ORDER BY channel NULLS FIRST,
         id NULLS FIRST,
         returns_ DESC
LIMIT 100;
