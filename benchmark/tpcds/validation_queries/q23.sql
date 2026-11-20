-- Item-date multiplicities are preserved; global maxima and customer cutoffs remain world-specific.
WITH frequent_stats AS (
SELECT itemdesc,
          i_item_sk item_sk,
          d_date solddate,
          tpcds_count(priv_hash(hash(ss_customer_sk)),1) cnt, tpcds_count(priv_hash(hash(ss_customer_sk)),1) AS presence
FROM main.store_sales,
        main.date_dim,
     (SELECT SUBSTRING(i_item_desc, 1, 30) itemdesc,
             *
      FROM main.item) sq1, (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE ss_customer_sk=asa_owner0.asa_key0 AND  ss_sold_date_sk = d_date_sk
     AND ss_item_sk = i_item_sk
     AND d_year IN (2000,
                    2000+1,
                    2000+2,
                    2000+3)
   GROUP BY itemdesc,
            i_item_sk,
            d_date

),
frequent_ss_items AS (SELECT itemdesc,item_sk,solddate,priv_select(18446744073709551615::UBIGINT,list_transform(cnt,lambda x: x>4)) AS membership FROM frequent_stats),
customer_period_stats AS (
SELECT c_customer_sk,
             tpcds_sum(priv_hash(hash(c_customer_sk)), ss_quantity*ss_sales_price) csales,
 tpcds_count(priv_hash(hash(c_customer_sk)),1) AS presence
FROM main.store_sales,
           main.customer,
           main.date_dim, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE c_customer_sk=asa_owner.asa_key AND ss_customer_sk = c_customer_sk
        AND ss_sold_date_sk = d_date_sk
        AND d_year IN (2000,
                       2000+1,
                       2000+2,
                       2000+3)
      GROUP BY c_customer_sk
),
customer_period AS (
SELECT world, c_customer_sk, csales[world+1] AS csales
FROM customer_period_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
max_sales AS (SELECT world,max(csales) AS maximum FROM customer_period GROUP BY world),
max_array AS (SELECT list(maximum ORDER BY w.world) AS thresholds FROM range(64) w(world) LEFT JOIN max_sales USING(world)),
best_stats AS (SELECT c_customer_sk,tpcds_sum(priv_hash(hash(c_customer_sk)),ss_quantity*ss_sales_price) AS ssales FROM main.store_sales JOIN main.customer ON ss_customer_sk=c_customer_sk GROUP BY c_customer_sk),
best_ss_customer AS (SELECT c_customer_sk,priv_select(priv_hash(hash(c_customer_sk)),list_transform(list_zip(ssales,thresholds),lambda x: x[1]>0.5*x[2])) AS membership FROM best_stats CROSS JOIN max_array),
channel0_stats AS (
SELECT c_last_name,
          c_first_name,
          tpcds_sum((priv_hash(hash(cs_bill_customer_sk)) & frequent_ss_items.membership & best_ss_customer.membership), cs_quantity*cs_list_price) sales, tpcds_count((priv_hash(hash(cs_bill_customer_sk)) & frequent_ss_items.membership & best_ss_customer.membership),1) AS presence
FROM main.catalog_sales,
        main.customer,
        main.date_dim,
        frequent_ss_items,
        best_ss_customer
WHERE  d_year = 2000
     AND d_moy = 2
     AND cs_sold_date_sk = d_date_sk
     AND cs_item_sk = item_sk
     AND cs_bill_customer_sk = best_ss_customer.c_customer_sk
     AND cs_bill_customer_sk = customer.c_customer_sk
   GROUP BY c_last_name,
            c_first_name

),
channel0 AS (
SELECT world, c_last_name, c_first_name, sales[world+1] AS sales
FROM channel0_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
channel1_stats AS (
 SELECT c_last_name,
                    c_first_name,
                    tpcds_sum((priv_hash(hash(ws_bill_customer_sk)) & frequent_ss_items.membership & best_ss_customer.membership), ws_quantity*ws_list_price) sales, tpcds_count((priv_hash(hash(ws_bill_customer_sk)) & frequent_ss_items.membership & best_ss_customer.membership),1) AS presence
FROM main.web_sales,
        main.customer,
        main.date_dim,
        frequent_ss_items,
        best_ss_customer
WHERE  d_year = 2000
     AND d_moy = 2
     AND ws_sold_date_sk = d_date_sk
     AND ws_item_sk = item_sk
     AND ws_bill_customer_sk = best_ss_customer.c_customer_sk
     AND ws_bill_customer_sk = customer.c_customer_sk
   GROUP BY c_last_name,
            c_first_name
),
channel1 AS (
SELECT world, c_last_name, c_first_name, sales[world+1] AS sales
FROM channel1_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
),
answers AS (SELECT * FROM channel0 UNION ALL SELECT * FROM channel1)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY c_last_name NULLS FIRST,c_first_name NULLS FIRST,sales NULLS FIRST)<=100 ORDER BY world,c_last_name NULLS FIRST,c_first_name NULLS FIRST,sales NULLS FIRST;
