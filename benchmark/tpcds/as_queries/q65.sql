-- Hand transcription of Q65's compiled PAC dataflow, for auditing (priv_rewrite=false).
-- This intentionally retains the scalarized inner SUM and majority filter from the compiler.
SELECT s_store_name, i_item_desc,
       CAST(as_noised(sc.revenue) AS DECIMAL(38,2)) AS revenue,
       i_current_price, i_wholesale_cost, i_brand
FROM main.store, main.item,
 (SELECT ss_store_sk, avg(revenue) AS ave
  FROM (SELECT ss_store_sk, ss_item_sk,
               as_noised_sum(priv_hash(hash(ss_customer_sk)),ss_sales_price) AS revenue
        FROM main.store_sales, main.date_dim
        WHERE ss_sold_date_sk=d_date_sk AND d_month_seq BETWEEN 1176 AND 1187
        GROUP BY ss_store_sk,ss_item_sk) sa
  GROUP BY ss_store_sk) sb,
 (SELECT ss_store_sk,ss_item_sk,
         as_sum(priv_hash(hash(ss_customer_sk)),ss_sales_price) AS revenue
  FROM main.store_sales, main.date_dim
  WHERE ss_sold_date_sk=d_date_sk AND d_month_seq BETWEEN 1176 AND 1187
  GROUP BY ss_store_sk,ss_item_sk) sc
WHERE sb.ss_store_sk=sc.ss_store_sk
  AND priv_filter_gte(CAST(0.1*sb.ave AS FLOAT),sc.revenue)
  AND s_store_sk=sc.ss_store_sk AND i_item_sk=sc.ss_item_sk
ORDER BY s_store_name NULLS FIRST,i_item_desc NULLS FIRST
LIMIT 100;
