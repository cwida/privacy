-- Correlated thresholds use only the matching world of the reduced return aggregates.
WITH return_stats AS (
SELECT cr_returning_customer_sk AS ctr_customer_sk ,
          ca_state AS ctr_state,
          tpcds_sum(priv_hash(hash(cr_returning_customer_sk)), cr_return_amt_inc_tax) AS ctr_total_return,
 tpcds_count(priv_hash(hash(cr_returning_customer_sk)),1) AS presence
FROM main.catalog_returns ,
        main.date_dim ,
        main.customer_address, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cr_returning_customer_sk=asa_owner.asa_key AND cr_returned_date_sk = d_date_sk
     AND d_year = 2000
     AND cr_returning_addr_sk = ca_address_sk
   GROUP BY cr_returning_customer_sk ,
            ca_state
), customer_total_return AS (
SELECT world, ctr_customer_sk, ctr_state, ctr_total_return[world+1] AS ctr_total_return
FROM return_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT ctr1.world AS world, c_customer_id,
       c_salutation,
       c_first_name,
       c_last_name,
       ca_street_number,
       ca_street_name ,
       ca_street_type,
       ca_suite_number,
       ca_city,
       ca_county,
       ca_state,
       ca_zip,
       ca_country,
       ca_gmt_offset ,
       ca_location_type,
       ctr_total_return
FROM customer_total_return ctr1 ,
     main.customer_address ,
     main.customer
WHERE ctr1.ctr_total_return >
    (SELECT avg(ctr_total_return)*1.2
     FROM customer_total_return ctr2
     WHERE ctr1.world=ctr2.world AND ctr1.ctr_state = ctr2.ctr_state)
  AND ca_address_sk = c_current_addr_sk
  AND ca_state = 'GA'
  AND ctr1.ctr_customer_sk = c_customer_sk
QUALIFY row_number() OVER (PARTITION BY ctr1.world ORDER BY c_customer_id,
         c_salutation,
         c_first_name,
         c_last_name,
         ca_street_number,
         ca_street_name ,
         ca_street_type,
         ca_suite_number,
         ca_city,
         ca_county,
         ca_state,
         ca_zip,
         ca_country,
         ca_gmt_offset ,
         ca_location_type,
         ctr_total_return) <= 100
ORDER BY ctr1.world, c_customer_id,
         c_salutation,
         c_first_name,
         c_last_name,
         ca_street_number,
         ca_street_name ,
         ca_street_type,
         ca_suite_number,
         ca_city,
         ca_county,
         ca_state,
         ca_zip,
         ca_country,
         ca_gmt_offset ,
         ca_location_type,
         ctr_total_return;
