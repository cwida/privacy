-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- Per-customer CTE totals stay scalar; only the correlated cross-customer AVG supplies a counter list.
WITH customer_total_return AS (SELECT cr_returning_customer_sk AS ctr_customer_sk ,
          ca_state AS ctr_state,
          sum(cr_return_amt_inc_tax) AS ctr_total_return
   FROM catalog_returns ,
        date_dim ,
        customer_address
   WHERE cr_returned_date_sk = d_date_sk
     AND d_year = 2000
     AND cr_returning_addr_sk = ca_address_sk
   GROUP BY cr_returning_customer_sk ,
            ca_state)
SELECT c_customer_id,
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
     customer_address ,
     customer
WHERE priv_filter_gt(CAST(CAST(ctr1.ctr_total_return AS DOUBLE) * 0.8333333333333334 AS FLOAT), (SELECT priv_div(as_sum(priv_hash(hash(ctr2.ctr_customer_sk)), ctr_total_return), as_count(priv_hash(hash(ctr2.ctr_customer_sk)), ctr_total_return))
     FROM customer_total_return ctr2
     WHERE ctr1.ctr_state = ctr2.ctr_state))
  AND ca_address_sk = c_current_addr_sk
  AND ca_state = 'GA'
  AND ctr1.ctr_customer_sk = c_customer_sk
ORDER BY c_customer_id,
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
LIMIT 100;
