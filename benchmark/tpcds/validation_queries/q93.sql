-- The reason predicate rejects unmatched returns; both owners must be present.
WITH input AS (
 SELECT ss_customer_sk,CASE WHEN sr_return_quantity IS NOT NULL THEN (ss_quantity-sr_return_quantity)*ss_sales_price ELSE ss_quantity*ss_sales_price END AS act_sales,
        (priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk))) AS membership
 FROM main.store_sales JOIN main.store_returns ON sr_item_sk=ss_item_sk AND sr_ticket_number=ss_ticket_number
 JOIN main.reason ON sr_reason_sk=r_reason_sk
 JOIN main.customer sale_owner ON ss_customer_sk=sale_owner.c_customer_sk
 JOIN main.customer return_owner ON sr_customer_sk=return_owner.c_customer_sk
 WHERE r_reason_desc='reason 28'
), stats AS (SELECT ss_customer_sk,tpcds_sum(membership,act_sales) AS sumsales,tpcds_count(membership,1) AS presence FROM input GROUP BY ss_customer_sk),
answers AS (SELECT world,ss_customer_sk,sumsales[world+1] AS sumsales FROM stats CROSS JOIN range(64) worlds(world) WHERE presence[world+1]>0)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY sumsales NULLS FIRST,ss_customer_sk NULLS FIRST)<=100 ORDER BY world,sumsales NULLS FIRST,ss_customer_sk NULLS FIRST;
