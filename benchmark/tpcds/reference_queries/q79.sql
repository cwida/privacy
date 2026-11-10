-- AS aggregation is shared; only the reduced per-ticket results are expanded by world.
WITH stats AS (
 SELECT ss_ticket_number,
        ss_customer_sk,
        store.s_city AS s_city,
        tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_coupon_amt) AS amt,
        tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_net_profit) AS profit,
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
     AND (household_demographics.hd_dep_count = 6
          OR household_demographics.hd_vehicle_count > 2)
     AND date_dim.d_dow = 1
     AND date_dim.d_year IN (1999,
                             1999+1,
                             1999+2)
     AND store.s_number_employees BETWEEN 200 AND 295
   GROUP BY ss_ticket_number,
            ss_customer_sk,
            ss_addr_sk,
            main.store.s_city
), expanded AS (
 SELECT world, ss_ticket_number, ss_customer_sk, s_city, amt[world+1] AS amt, profit[world+1] AS profit
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1]>0
)
SELECT world, c_last_name,
       c_first_name,
       SUBSTRING(s_city,1,30),
       ss_ticket_number,
       amt,
       profit
FROM
  expanded  ms,
     main.customer
WHERE ss_customer_sk = c_customer_sk
QUALIFY row_number() OVER (PARTITION BY world ORDER BY c_last_name  NULLS FIRST,
         c_first_name  NULLS FIRST,
         SUBSTRING(s_city,1,30)  NULLS FIRST,
         profit NULLS FIRST,
         ss_ticket_number) <= 100
ORDER BY world, c_last_name  NULLS FIRST,
         c_first_name  NULLS FIRST,
         SUBSTRING(s_city,1,30)  NULLS FIRST,
         profit NULLS FIRST,
         ss_ticket_number;
