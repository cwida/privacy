-- Scalar AS arrays are combined only after their independent shared scans.
WITH raw0 AS (
SELECT tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) amc
   FROM main.web_sales,
        main.household_demographics,
        main.time_dim,
        main.web_page, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND  ws_sold_time_sk = time_dim.t_time_sk
     AND ws_ship_hdemo_sk = household_demographics.hd_demo_sk
     AND ws_web_page_sk = web_page.wp_web_page_sk
     AND time_dim.t_hour BETWEEN 8 AND 8+1
     AND household_demographics.hd_dep_count = 6
     AND web_page.wp_char_count BETWEEN 5000 AND 5200
),
raw1 AS (
SELECT tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) pmc
   FROM main.web_sales,
        main.household_demographics,
        main.time_dim,
        main.web_page, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND  ws_sold_time_sk = time_dim.t_time_sk
     AND ws_ship_hdemo_sk = household_demographics.hd_demo_sk
     AND ws_web_page_sk = web_page.wp_web_page_sk
     AND time_dim.t_hour BETWEEN 19 AND 19+1
     AND household_demographics.hd_dep_count = 6
     AND web_page.wp_char_count BETWEEN 5000 AND 5200
),
p AS (SELECT world, raw0.amc[world+1] AS amc, raw1.pmc[world+1] AS pmc FROM raw0 CROSS JOIN raw1 CROSS JOIN range(64) worlds(world))
SELECT world, case when p.pmc=0 then null else cast(p.amc AS decimal(15,4))/cast(p.pmc AS decimal(15,4)) end am_pm_ratio
 FROM p ORDER BY world;
