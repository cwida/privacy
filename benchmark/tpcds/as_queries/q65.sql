-- Hand transcription of the compiled PAC dataflow (priv_rewrite=false).
-- AVG consumes the per-world inner SUMs before the majority predicate.
WITH revenue AS (
 SELECT ss_store_sk,ss_item_sk,
        as_sum(priv_hash(hash(ss_customer_sk)),ss_sales_price) AS revenue
 FROM main.store_sales,main.date_dim
 WHERE ss_sold_date_sk=d_date_sk AND d_month_seq BETWEEN 1176 AND 1187
 GROUP BY ss_store_sk,ss_item_sk
), averages AS (
 SELECT ss_store_sk,
        list_transform(list_zip(as_sum(revenue),as_count(revenue)),lambda x:x[1]/x[2]) AS ave
 FROM revenue GROUP BY ss_store_sk
)
SELECT s_store_name,i_item_desc,CAST(as_noised(sc.revenue) AS DECIMAL(38,2)) AS revenue,
       i_current_price,i_wholesale_cost,i_brand
FROM main.store,main.item,revenue sc,averages sb
WHERE sb.ss_store_sk=sc.ss_store_sk
  AND priv_filter(list_transform(list_zip(sc.revenue,sb.ave),lambda x:CAST(x[1] AS FLOAT)<=CAST(0.1*CAST(x[2] AS DOUBLE) AS FLOAT)))
  AND s_store_sk=sc.ss_store_sk AND i_item_sk=sc.ss_item_sk
ORDER BY s_store_name NULLS FIRST,i_item_desc NULLS FIRST
LIMIT 100;
