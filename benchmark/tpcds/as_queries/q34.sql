-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- The terminal BETWEEN uses two separate PAC majority comparisons, not one per-world conjunction.
SELECT c_last_name ,
       c_first_name ,
       c_salutation ,
       c_preferred_cust_flag ,
       ss_ticket_number ,
       CAST(as_noised(cnt) AS BIGINT) AS cnt
FROM
  (SELECT ss_ticket_number ,
          ss_customer_sk ,
          as_count(priv_hash(hash(ss_customer_sk)), 1::BIGINT) cnt
   FROM store_sales,
        date_dim,
        store,
        household_demographics
   WHERE store_sales.ss_sold_date_sk = date_dim.d_date_sk
     AND store_sales.ss_store_sk = store.s_store_sk
     AND store_sales.ss_hdemo_sk = household_demographics.hd_demo_sk
     AND (date_dim.d_dom BETWEEN 1 AND 3
          OR date_dim.d_dom BETWEEN 25 AND 28)
     AND (household_demographics.hd_buy_potential = '>10000'
          OR household_demographics.hd_buy_potential = 'Unknown')
     AND household_demographics.hd_vehicle_count > 0
     AND (CASE
              WHEN household_demographics.hd_vehicle_count > 0 THEN (household_demographics.hd_dep_count*1.000)/ household_demographics.hd_vehicle_count
              ELSE NULL
          END) > 1.2
     AND date_dim.d_year IN (1999,
                             1999+1,
                             1999+2)
     AND store.s_county = 'Williamson County'
   GROUP BY ss_ticket_number,
            ss_customer_sk) dn,
     customer
WHERE ss_customer_sk = c_customer_sk
  AND priv_filter_lte(15::FLOAT, cnt) AND priv_filter_gte(20::FLOAT, cnt)
ORDER BY c_last_name NULLS FIRST,
         c_first_name NULLS FIRST,
         c_salutation NULLS FIRST,
         c_preferred_cust_flag DESC NULLS FIRST,
         ss_ticket_number NULLS FIRST;
