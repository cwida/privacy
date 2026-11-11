-- Sale and return ownership masks are intersected before the shared inner aggregation.
WITH ssales_stats AS (
SELECT c_last_name,
          c_first_name,
          s_store_name,
          ca_state,
          s_state,
          i_color,
          i_current_price,
          i_manager_id,
          i_units,
          i_size,
          tpcds_sum((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk))), ss_net_paid) netpaid, tpcds_count((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk))),1) AS presence
FROM main.store_sales,
        main.store_returns,
        main.store,
        main.item,
        main.customer,
        main.customer_address, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_return_owner
WHERE sr_customer_sk=asa_return_owner.asa_key AND  ss_ticket_number = sr_ticket_number
     AND ss_item_sk = sr_item_sk
     AND ss_customer_sk = c_customer_sk
     AND ss_item_sk = i_item_sk
     AND ss_store_sk = s_store_sk
     AND c_current_addr_sk = ca_address_sk
     AND c_birth_country <> upper(ca_country)
     AND s_zip = ca_zip
     AND s_market_id=8
   GROUP BY c_last_name,
            c_first_name,
            s_store_name,
            ca_state,
            s_state,
            i_color,
            i_current_price,
            i_manager_id,
            i_units,
            i_size
), ssales AS (
SELECT world, c_last_name, c_first_name, s_store_name, ca_state, s_state, i_color, i_current_price, i_manager_id, i_units, i_size, netpaid[world+1] AS netpaid
FROM ssales_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT outer_ss.world AS world, c_last_name,
       c_first_name,
       s_store_name,
       sum(netpaid) paid
FROM ssales outer_ss
WHERE i_color = 'peach'
GROUP BY outer_ss.world, c_last_name,
         c_first_name,
         s_store_name
HAVING sum(netpaid) >
  (SELECT 0.05*avg(netpaid)
   FROM ssales inner_ss WHERE inner_ss.world=outer_ss.world)
ORDER BY outer_ss.world, c_last_name,
         c_first_name,
         s_store_name;
