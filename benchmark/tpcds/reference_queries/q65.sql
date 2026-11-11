-- Inner item revenues are shared across both outer uses; AVG remains world-specific.
WITH revenue_stats AS (
SELECT ss_store_sk, ss_item_sk, tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_sales_price) AS revenue,
 tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales, main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND ss_sold_date_sk=d_date_sk AND d_month_seq BETWEEN 1176 AND 1176+11
GROUP BY ss_store_sk, ss_item_sk
), sc AS (
SELECT world, ss_store_sk, ss_item_sk, revenue[world+1] AS revenue
FROM revenue_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
), sb AS (SELECT world, ss_store_sk, avg(revenue) AS ave FROM sc GROUP BY world, ss_store_sk)
SELECT sc.world AS world, s_store_name, i_item_desc, sc.revenue, i_current_price, i_wholesale_cost, i_brand
FROM main.store, main.item, sb, sc
WHERE sc.world=sb.world AND  sb.ss_store_sk=sc.ss_store_sk AND sc.revenue<=0.1*sb.ave AND s_store_sk=sc.ss_store_sk AND i_item_sk=sc.ss_item_sk
QUALIFY row_number() OVER (PARTITION BY sc.world ORDER BY s_store_name NULLS FIRST, i_item_desc NULLS FIRST) <= 100
ORDER BY sc.world, s_store_name NULLS FIRST, i_item_desc NULLS FIRST;
