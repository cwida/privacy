-- Shared monthly AS aggregates retain each channel's original price/profit expression.
WITH stats AS (

     select
  w_warehouse_name
  ,w_warehouse_sq_ft
  ,w_city
  ,w_county
  ,w_state
  ,w_country
  ,'DHL,BARIAN' as ship_carriers
       ,d_year as year_
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 1
    then ws_ext_sales_price* ws_quantity else 0 end) as jan_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 2
    then ws_ext_sales_price* ws_quantity else 0 end) as feb_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 3
    then ws_ext_sales_price* ws_quantity else 0 end) as mar_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 4
    then ws_ext_sales_price* ws_quantity else 0 end) as apr_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 5
    then ws_ext_sales_price* ws_quantity else 0 end) as may_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 6
    then ws_ext_sales_price* ws_quantity else 0 end) as jun_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 7
    then ws_ext_sales_price* ws_quantity else 0 end) as jul_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 8
    then ws_ext_sales_price* ws_quantity else 0 end) as aug_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 9
    then ws_ext_sales_price* ws_quantity else 0 end) as sep_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 10
    then ws_ext_sales_price* ws_quantity else 0 end) as oct_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 11
    then ws_ext_sales_price* ws_quantity else 0 end) as nov_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 12
    then ws_ext_sales_price* ws_quantity else 0 end) as dec_sales
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 1
    then ws_net_paid * ws_quantity else 0 end) as jan_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 2
    then ws_net_paid * ws_quantity else 0 end) as feb_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 3
    then ws_net_paid * ws_quantity else 0 end) as mar_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 4
    then ws_net_paid * ws_quantity else 0 end) as apr_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 5
    then ws_net_paid * ws_quantity else 0 end) as may_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 6
    then ws_net_paid * ws_quantity else 0 end) as jun_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 7
    then ws_net_paid * ws_quantity else 0 end) as jul_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 8
    then ws_net_paid * ws_quantity else 0 end) as aug_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 9
    then ws_net_paid * ws_quantity else 0 end) as sep_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 10
    then ws_net_paid * ws_quantity else 0 end) as oct_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 11
    then ws_net_paid * ws_quantity else 0 end) as nov_net
  ,tpcds_sum(priv_hash(hash(ws_bill_customer_sk)), case when d_moy = 12
    then ws_net_paid * ws_quantity else 0 end) as dec_net,
 tpcds_count(priv_hash(hash(ws_bill_customer_sk)),1) AS presence
from
          main.web_sales
         ,main.warehouse
         ,main.date_dim
         ,main.time_dim
    ,main.ship_mode, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ws_bill_customer_sk=asa_owner.asa_key AND ws_warehouse_sk =  w_warehouse_sk
        and ws_sold_date_sk = d_date_sk
        and ws_sold_time_sk = t_time_sk
  and ws_ship_mode_sk = sm_ship_mode_sk
        and d_year = 2001
  and t_time between 30838 and 30838+28800
  and sm_carrier in ('DHL','BARIAN')
     group by
        w_warehouse_name
  ,w_warehouse_sq_ft
  ,w_city
  ,w_county
  ,w_state
  ,w_country
       ,d_year

UNION ALL

     select
  w_warehouse_name
  ,w_warehouse_sq_ft
  ,w_city
  ,w_county
  ,w_state
  ,w_country
  ,'DHL,BARIAN' as ship_carriers
       ,d_year as year_
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 1
    then cs_sales_price* cs_quantity else 0 end) as jan_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 2
    then cs_sales_price* cs_quantity else 0 end) as feb_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 3
    then cs_sales_price* cs_quantity else 0 end) as mar_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 4
    then cs_sales_price* cs_quantity else 0 end) as apr_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 5
    then cs_sales_price* cs_quantity else 0 end) as may_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 6
    then cs_sales_price* cs_quantity else 0 end) as jun_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 7
    then cs_sales_price* cs_quantity else 0 end) as jul_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 8
    then cs_sales_price* cs_quantity else 0 end) as aug_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 9
    then cs_sales_price* cs_quantity else 0 end) as sep_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 10
    then cs_sales_price* cs_quantity else 0 end) as oct_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 11
    then cs_sales_price* cs_quantity else 0 end) as nov_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 12
    then cs_sales_price* cs_quantity else 0 end) as dec_sales
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 1
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as jan_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 2
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as feb_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 3
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as mar_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 4
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as apr_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 5
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as may_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 6
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as jun_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 7
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as jul_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 8
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as aug_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 9
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as sep_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 10
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as oct_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 11
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as nov_net
  ,tpcds_sum(priv_hash(hash(cs_bill_customer_sk)), case when d_moy = 12
    then cs_net_paid_inc_tax * cs_quantity else 0 end) as dec_net,
 tpcds_count(priv_hash(hash(cs_bill_customer_sk)),1) AS presence
from
          main.catalog_sales
         ,main.warehouse
         ,main.date_dim
         ,main.time_dim
   ,main.ship_mode, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE cs_bill_customer_sk=asa_owner.asa_key AND cs_warehouse_sk =  w_warehouse_sk
        and cs_sold_date_sk = d_date_sk
        and cs_sold_time_sk = t_time_sk
  and cs_ship_mode_sk = sm_ship_mode_sk
        and d_year = 2001
  and t_time between 30838 AND 30838+28800
  and sm_carrier in ('DHL','BARIAN')
     group by
        w_warehouse_name
  ,w_warehouse_sq_ft
  ,w_city
  ,w_county
  ,w_state
  ,w_country
       ,d_year

), expanded AS (
SELECT world, w_warehouse_name, w_warehouse_sq_ft, w_city, w_county, w_state, w_country, ship_carriers, year_, jan_sales[world+1] AS jan_sales, feb_sales[world+1] AS feb_sales, mar_sales[world+1] AS mar_sales, apr_sales[world+1] AS apr_sales, may_sales[world+1] AS may_sales, jun_sales[world+1] AS jun_sales, jul_sales[world+1] AS jul_sales, aug_sales[world+1] AS aug_sales, sep_sales[world+1] AS sep_sales, oct_sales[world+1] AS oct_sales, nov_sales[world+1] AS nov_sales, dec_sales[world+1] AS dec_sales, jan_net[world+1] AS jan_net, feb_net[world+1] AS feb_net, mar_net[world+1] AS mar_net, apr_net[world+1] AS apr_net, may_net[world+1] AS may_net, jun_net[world+1] AS jun_net, jul_net[world+1] AS jul_net, aug_net[world+1] AS aug_net, sep_net[world+1] AS sep_net, oct_net[world+1] AS oct_net, nov_net[world+1] AS nov_net, dec_net[world+1] AS dec_net
FROM stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
), answers AS (
select world,
         w_warehouse_name
  ,w_warehouse_sq_ft
  ,w_city
  ,w_county
  ,w_state
  ,w_country
        ,ship_carriers
        ,year_
  ,sum(jan_sales) as jan_sales
  ,sum(feb_sales) as feb_sales
  ,sum(mar_sales) as mar_sales
  ,sum(apr_sales) as apr_sales
  ,sum(may_sales) as may_sales
  ,sum(jun_sales) as jun_sales
  ,sum(jul_sales) as jul_sales
  ,sum(aug_sales) as aug_sales
  ,sum(sep_sales) as sep_sales
  ,sum(oct_sales) as oct_sales
  ,sum(nov_sales) as nov_sales
  ,sum(dec_sales) as dec_sales
  ,sum(jan_sales/w_warehouse_sq_ft) as jan_sales_per_sq_foot
  ,sum(feb_sales/w_warehouse_sq_ft) as feb_sales_per_sq_foot
  ,sum(mar_sales/w_warehouse_sq_ft) as mar_sales_per_sq_foot
  ,sum(apr_sales/w_warehouse_sq_ft) as apr_sales_per_sq_foot
  ,sum(may_sales/w_warehouse_sq_ft) as may_sales_per_sq_foot
  ,sum(jun_sales/w_warehouse_sq_ft) as jun_sales_per_sq_foot
  ,sum(jul_sales/w_warehouse_sq_ft) as jul_sales_per_sq_foot
  ,sum(aug_sales/w_warehouse_sq_ft) as aug_sales_per_sq_foot
  ,sum(sep_sales/w_warehouse_sq_ft) as sep_sales_per_sq_foot
  ,sum(oct_sales/w_warehouse_sq_ft) as oct_sales_per_sq_foot
  ,sum(nov_sales/w_warehouse_sq_ft) as nov_sales_per_sq_foot
  ,sum(dec_sales/w_warehouse_sq_ft) as dec_sales_per_sq_foot
  ,sum(jan_net) as jan_net
  ,sum(feb_net) as feb_net
  ,sum(mar_net) as mar_net
  ,sum(apr_net) as apr_net
  ,sum(may_net) as may_net
  ,sum(jun_net) as jun_net
  ,sum(jul_net) as jul_net
  ,sum(aug_net) as aug_net
  ,sum(sep_net) as sep_net
  ,sum(oct_net) as oct_net
  ,sum(nov_net) as nov_net
  ,sum(dec_net) as dec_net
 from expanded  x
 GROUP BY world, w_warehouse_name
  ,w_warehouse_sq_ft
  ,w_city
  ,w_county
  ,w_state
  ,w_country
  ,ship_carriers
       ,year_

)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY w_warehouse_name NULLS FIRST)<=100 ORDER BY world,w_warehouse_name NULLS FIRST;
