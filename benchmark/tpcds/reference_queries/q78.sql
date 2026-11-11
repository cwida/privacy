-- Return anti-memberships feed the per-customer/item/year AS sums before channel joins.
WITH ws_return_presence AS (SELECT wr_order_number AS order_number,wr_item_sk AS item_sk,bit_or(priv_hash(hash(wr_returning_customer_sk))) AS membership FROM main.web_returns JOIN main.customer ON wr_returning_customer_sk=c_customer_sk GROUP BY wr_order_number,wr_item_sk),
ws_stats AS (
SELECT d_year AS ws_sold_year,ws_item_sk,ws_bill_customer_sk AS ws_customer_sk,
 tpcds_sum((priv_hash(hash(ws_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),ws_quantity) AS ws_qty,tpcds_sum((priv_hash(hash(ws_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),ws_wholesale_cost) AS ws_wc,tpcds_sum((priv_hash(hash(ws_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),ws_sales_price) AS ws_sp,tpcds_count((priv_hash(hash(ws_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),1) AS presence
FROM main.web_sales JOIN main.customer ON ws_bill_customer_sk=c_customer_sk JOIN main.date_dim ON ws_sold_date_sk=d_date_sk
LEFT JOIN ws_return_presence ret ON ws_order_number=ret.order_number AND ws_item_sk=ret.item_sk
GROUP BY d_year,ws_item_sk,ws_bill_customer_sk
),
ws AS (
SELECT world, ws_sold_year, ws_item_sk, ws_customer_sk, ws_qty[world+1] AS ws_qty, ws_wc[world+1] AS ws_wc, ws_sp[world+1] AS ws_sp
FROM ws_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
cs_return_presence AS (SELECT cr_order_number AS order_number,cr_item_sk AS item_sk,bit_or(priv_hash(hash(cr_returning_customer_sk))) AS membership FROM main.catalog_returns JOIN main.customer ON cr_returning_customer_sk=c_customer_sk GROUP BY cr_order_number,cr_item_sk),
cs_stats AS (
SELECT d_year AS cs_sold_year,cs_item_sk,cs_bill_customer_sk AS cs_customer_sk,
 tpcds_sum((priv_hash(hash(cs_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),cs_quantity) AS cs_qty,tpcds_sum((priv_hash(hash(cs_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),cs_wholesale_cost) AS cs_wc,tpcds_sum((priv_hash(hash(cs_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),cs_sales_price) AS cs_sp,tpcds_count((priv_hash(hash(cs_bill_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),1) AS presence
FROM main.catalog_sales JOIN main.customer ON cs_bill_customer_sk=c_customer_sk JOIN main.date_dim ON cs_sold_date_sk=d_date_sk
LEFT JOIN cs_return_presence ret ON cs_order_number=ret.order_number AND cs_item_sk=ret.item_sk
GROUP BY d_year,cs_item_sk,cs_bill_customer_sk
),
cs AS (
SELECT world, cs_sold_year, cs_item_sk, cs_customer_sk, cs_qty[world+1] AS cs_qty, cs_wc[world+1] AS cs_wc, cs_sp[world+1] AS cs_sp
FROM cs_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
ss_return_presence AS (SELECT sr_ticket_number AS order_number,sr_item_sk AS item_sk,bit_or(priv_hash(hash(sr_customer_sk))) AS membership FROM main.store_returns JOIN main.customer ON sr_customer_sk=c_customer_sk GROUP BY sr_ticket_number,sr_item_sk),
ss_stats AS (
SELECT d_year AS ss_sold_year,ss_item_sk,ss_customer_sk AS ss_customer_sk,
 tpcds_sum((priv_hash(hash(ss_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),ss_quantity) AS ss_qty,tpcds_sum((priv_hash(hash(ss_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),ss_wholesale_cost) AS ss_wc,tpcds_sum((priv_hash(hash(ss_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),ss_sales_price) AS ss_sp,tpcds_count((priv_hash(hash(ss_customer_sk)) & ~coalesce(ret.membership,0::UBIGINT)),1) AS presence
FROM main.store_sales JOIN main.customer ON ss_customer_sk=c_customer_sk JOIN main.date_dim ON ss_sold_date_sk=d_date_sk
LEFT JOIN ss_return_presence ret ON ss_ticket_number=ret.order_number AND ss_item_sk=ret.item_sk
GROUP BY d_year,ss_item_sk,ss_customer_sk
),
ss AS (
SELECT world, ss_sold_year, ss_item_sk, ss_customer_sk, ss_qty[world+1] AS ss_qty, ss_wc[world+1] AS ss_wc, ss_sp[world+1] AS ss_sp
FROM ss_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT ss.world AS world, ss_sold_year,
       ss_item_sk,
       ss_customer_sk,
       round((ss_qty*1.00)/(coalesce(ws_qty,0)+coalesce(cs_qty,0)),2) ratio,
       ss_qty store_qty,
       ss_wc store_wholesale_cost,
       ss_sp store_sales_price,
       coalesce(ws_qty,0)+coalesce(cs_qty,0) other_chan_qty,
       coalesce(ws_wc,0)+coalesce(cs_wc,0) other_chan_wholesale_cost,
       coalesce(ws_sp,0)+coalesce(cs_sp,0) other_chan_sales_price
FROM ss
LEFT JOIN ws ON (ss.world=ws.world AND ws_sold_year=ss_sold_year
                 AND ws_item_sk=ss_item_sk
                 AND ws_customer_sk=ss_customer_sk)
LEFT JOIN cs ON (ss.world=cs.world AND cs_sold_year=ss_sold_year
                 AND cs_item_sk=ss_item_sk
                 AND cs_customer_sk=ss_customer_sk)
WHERE (coalesce(ws_qty,0)>0
       OR coalesce(cs_qty, 0)>0)
  AND ss_sold_year=2000
QUALIFY row_number() OVER (PARTITION BY ss.world ORDER BY ss_sold_year,
         ss_item_sk,
         ss_customer_sk,
         ss_qty DESC,
         ss_wc DESC,
         ss_sp DESC,
         other_chan_qty,
         other_chan_wholesale_cost,
         other_chan_sales_price,
         ratio)<=100
ORDER BY ss.world,ss_sold_year,
         ss_item_sk,
         ss_customer_sk,
         ss_qty DESC,
         ss_wc DESC,
         ss_sp DESC,
         other_chan_qty,
         other_chan_wholesale_cost,
         other_chan_sales_price,
         ratio;
