-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT cc_call_center_id AS call_center,
        cc_name AS call_center_name,
        cc_manager AS manager,
        tpcds_sum((priv_hash(hash(cr_returning_customer_sk))), cr_net_loss) AS returns_loss,
        tpcds_count((priv_hash(hash(cr_returning_customer_sk))), 1) AS presence
 FROM main.call_center,
     main.catalog_returns,
     main.date_dim,
     main.customer,
     main.customer_address,
     main.customer_demographics,
     main.household_demographics,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE cr_returning_customer_sk = asa_owner0.asa_key0
  AND cr_call_center_sk = cc_call_center_sk
  AND cr_returned_date_sk = d_date_sk
  AND cr_returning_customer_sk= c_customer_sk
  AND cd_demo_sk = c_current_cdemo_sk
  AND hd_demo_sk = c_current_hdemo_sk
  AND ca_address_sk = c_current_addr_sk
  AND d_year = 1998
  AND d_moy = 11
  AND ((cd_marital_status = 'M'
        AND cd_education_status = 'Unknown') or(cd_marital_status = 'W'
                                                AND cd_education_status = 'Advanced Degree'))
  AND hd_buy_potential LIKE 'Unknown%'
  AND ca_gmt_offset = -7
GROUP BY cc_call_center_id,
         cc_name,
         cc_manager,
         cd_marital_status,
         cd_education_status
), answers AS (
 SELECT world, call_center,
        call_center_name,
        manager,
        returns_loss[world+1] AS returns_loss
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
ORDER BY world, returns_loss DESC;
