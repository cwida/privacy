-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- Evaluate the NULL guard and DECIMAL ratio lane-wise before FLOAT-list finalization.
SELECT CAST(as_noised(list_transform(list_zip(amc,pmc), lambda x: CASE WHEN x[2]=0 THEN NULL ELSE CAST(x[1] AS DECIMAL(15,4))/CAST(x[2] AS DECIMAL(15,4)) END)::FLOAT[]) AS DOUBLE) AS am_pm_ratio
FROM (SELECT as_count(priv_hash(hash(ws_bill_customer_sk)), 1::BIGINT) amc
   FROM web_sales,
        household_demographics,
        time_dim,
        web_page
   WHERE ws_sold_time_sk = time_dim.t_time_sk
     AND ws_ship_hdemo_sk = household_demographics.hd_demo_sk
     AND ws_web_page_sk = web_page.wp_web_page_sk
     AND time_dim.t_hour BETWEEN 8 AND 8+1
     AND household_demographics.hd_dep_count = 6
     AND web_page.wp_char_count BETWEEN 5000 AND 5200) at_, (SELECT as_count(priv_hash(hash(ws_bill_customer_sk)), 1::BIGINT) pmc
   FROM web_sales,
        household_demographics,
        time_dim,
        web_page
   WHERE ws_sold_time_sk = time_dim.t_time_sk
     AND ws_ship_hdemo_sk = household_demographics.hd_demo_sk
     AND ws_web_page_sk = web_page.wp_web_page_sk
     AND time_dim.t_hour BETWEEN 19 AND 19+1
     AND household_demographics.hd_dep_count = 6
     AND web_page.wp_char_count BETWEEN 5000 AND 5200)  pt
ORDER BY am_pm_ratio
LIMIT 100;
