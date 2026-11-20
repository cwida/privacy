-- AS aggregation is shared; only the reduced per-ticket results are expanded by world.
WITH stats AS (
 SELECT ss_ticket_number,
        ss_customer_sk,
        ca_city AS bought_city,
        tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_sales_price) AS extended_price,
        tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_list_price) AS list_price,
        tpcds_sum(priv_hash(hash(ss_customer_sk)), ss_ext_tax) AS extended_tax,
        tpcds_count(priv_hash(hash(ss_customer_sk)), 1) AS presence
 FROM main.store_sales,
        main.date_dim,
        main.store,
        main.household_demographics,
        main.customer_address,
        (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key
 AND store_sales.ss_sold_date_sk = date_dim.d_date_sk
     AND store_sales.ss_store_sk = store.s_store_sk
     AND store_sales.ss_hdemo_sk = household_demographics.hd_demo_sk
     AND store_sales.ss_addr_sk = customer_address.ca_address_sk
     AND date_dim.d_dom BETWEEN 1 AND 2
     AND (household_demographics.hd_dep_count = 4
          OR household_demographics.hd_vehicle_count= 3)
     AND date_dim.d_year IN (1999,
                             1999+1,
                             1999+2)
     AND store.s_city IN ('Fairview',
                          'Midway')
   GROUP BY ss_ticket_number,
            ss_customer_sk,
            ss_addr_sk,
            ca_city
), expanded AS (
 SELECT world, ss_ticket_number, ss_customer_sk, bought_city, extended_price[world+1] AS extended_price, list_price[world+1] AS list_price, extended_tax[world+1] AS extended_tax
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1]>0
)
SELECT world, c_last_name,
       c_first_name,
       ca_city,
       bought_city,
       ss_ticket_number,
       extended_price,
       extended_tax,
       list_price
FROM
  expanded  dn,
     main.customer,
     main.customer_address current_addr
WHERE ss_customer_sk = c_customer_sk
  AND customer.c_current_addr_sk = current_addr.ca_address_sk
  AND current_addr.ca_city <> bought_city
QUALIFY row_number() OVER (PARTITION BY world ORDER BY c_last_name NULLS FIRST,
         ss_ticket_number NULLS FIRST) <= 100
ORDER BY world, c_last_name NULLS FIRST,
         ss_ticket_number NULLS FIRST;
