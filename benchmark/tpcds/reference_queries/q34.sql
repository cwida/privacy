-- AS aggregation is shared; only the reduced per-ticket results are expanded by world.
WITH stats AS (
 SELECT ss_ticket_number,
        ss_customer_sk,
        tpcds_count(priv_hash(hash(ss_customer_sk)), 1) AS cnt,
        tpcds_count(priv_hash(hash(ss_customer_sk)), 1) AS presence
 FROM main.store_sales,
        main.date_dim,
        main.store,
        main.household_demographics,
        (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key
 AND store_sales.ss_sold_date_sk = date_dim.d_date_sk
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
            ss_customer_sk
), expanded AS (
 SELECT world, ss_ticket_number, ss_customer_sk, cnt[world+1] AS cnt
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1]>0
)
SELECT world, c_last_name ,
       c_first_name ,
       c_salutation ,
       c_preferred_cust_flag ,
       ss_ticket_number ,
       cnt
FROM
  expanded  dn,
     main.customer
WHERE ss_customer_sk = c_customer_sk
  AND cnt BETWEEN 15 AND 20
ORDER BY world, c_last_name NULLS FIRST,
         c_first_name NULLS FIRST,
         c_salutation NULLS FIRST,
         c_preferred_cust_flag DESC NULLS FIRST,
         ss_ticket_number NULLS FIRST;
