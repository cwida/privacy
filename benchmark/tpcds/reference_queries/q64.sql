-- The first stochastic HAVING is folded into membership before the cross-sales aggregate.
WITH cs_ui_stats AS (
SELECT cs_item_sk,
          tpcds_sum((priv_hash(hash(cs_bill_customer_sk)) & priv_hash(hash(cr_returning_customer_sk))), cs_ext_list_price) AS sale,
          tpcds_sum((priv_hash(hash(cs_bill_customer_sk)) & priv_hash(hash(cr_returning_customer_sk))), cr_refunded_cash+cr_reversed_charge+cr_store_credit) AS refund, tpcds_count((priv_hash(hash(cs_bill_customer_sk)) & priv_hash(hash(cr_returning_customer_sk))),1) AS presence
FROM main.catalog_sales,
        main.catalog_returns, (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0, (SELECT c_customer_sk AS asa_key1 FROM main.customer) asa_owner1
WHERE cs_bill_customer_sk=asa_owner0.asa_key0 AND cr_returning_customer_sk=asa_owner1.asa_key1 AND  cs_item_sk = cr_item_sk
     AND cs_order_number = cr_order_number
   GROUP BY cs_item_sk

),
cs_ui AS (SELECT cs_item_sk,priv_select(18446744073709551615::UBIGINT,list_transform(list_zip(sale,refund),lambda x: x[1]>2*x[2])) AS membership FROM cs_ui_stats),
cross_sales_stats AS (
SELECT i_product_name product_name,
          i_item_sk item_sk,
          s_store_name store_name,
          s_zip store_zip,
          ad1.ca_street_number b_street_number,
          ad1.ca_street_name b_street_name,
          ad1.ca_city b_city,
          ad1.ca_zip b_zip,
          ad2.ca_street_number c_street_number,
          ad2.ca_street_name c_street_name,
          ad2.ca_city c_city,
          ad2.ca_zip c_zip,
          d1.d_year AS syear,
          d2.d_year AS fsyear,
          d3.d_year s2year,
          tpcds_count((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk)) & cs_ui.membership),1) cnt,
          tpcds_sum((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk)) & cs_ui.membership), ss_wholesale_cost) s1,
          tpcds_sum((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk)) & cs_ui.membership), ss_list_price) s2,
          tpcds_sum((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk)) & cs_ui.membership), ss_coupon_amt) s3, tpcds_count((priv_hash(hash(ss_customer_sk)) & priv_hash(hash(sr_customer_sk)) & cs_ui.membership),1) AS presence
FROM main.store_sales,
        main.store_returns,
        cs_ui,
        main.date_dim d1,
        main.date_dim d2,
        main.date_dim d3,
        main.store,
        main.customer,
        main.customer_demographics cd1,
        main.customer_demographics cd2,
        main.promotion,
        main.household_demographics hd1,
        main.household_demographics hd2,
        main.customer_address ad1,
        main.customer_address ad2,
        main.income_band ib1,
        main.income_band ib2,
        main.item, (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE sr_customer_sk=asa_owner0.asa_key0 AND  ss_store_sk = s_store_sk
     AND ss_sold_date_sk = d1.d_date_sk
     AND ss_customer_sk = c_customer_sk
     AND ss_cdemo_sk= cd1.cd_demo_sk
     AND ss_hdemo_sk = hd1.hd_demo_sk
     AND ss_addr_sk = ad1.ca_address_sk
     AND ss_item_sk = i_item_sk
     AND ss_item_sk = sr_item_sk
     AND ss_ticket_number = sr_ticket_number
     AND ss_item_sk = cs_ui.cs_item_sk
     AND c_current_cdemo_sk = cd2.cd_demo_sk
     AND c_current_hdemo_sk = hd2.hd_demo_sk
     AND c_current_addr_sk = ad2.ca_address_sk
     AND c_first_sales_date_sk = d2.d_date_sk
     AND c_first_shipto_date_sk = d3.d_date_sk
     AND ss_promo_sk = p_promo_sk
     AND hd1.hd_income_band_sk = ib1.ib_income_band_sk
     AND hd2.hd_income_band_sk = ib2.ib_income_band_sk
     AND cd1.cd_marital_status <> cd2.cd_marital_status
     AND i_color IN ('purple',
                     'burlywood',
                     'indian',
                     'spring',
                     'floral',
                     'medium')
     AND i_current_price BETWEEN 64 AND 64 + 10
     AND i_current_price BETWEEN 64 + 1 AND 64 + 15
   GROUP BY i_product_name,
            i_item_sk,
            s_store_name,
            s_zip,
            ad1.ca_street_number,
            ad1.ca_street_name,
            ad1.ca_city,
            ad1.ca_zip,
            ad2.ca_street_number,
            ad2.ca_street_name,
            ad2.ca_city,
            ad2.ca_zip,
            d1.d_year,
            d2.d_year,
            d3.d_year
),
cross_sales AS (
SELECT world, product_name, item_sk, store_name, store_zip, b_street_number, b_street_name, b_city, b_zip, c_street_number, c_street_name, c_city, c_zip, syear, fsyear, s2year, cnt[world+1] AS cnt, s1[world+1] AS s1, s2[world+1] AS s2, s3[world+1] AS s3
FROM cross_sales_stats CROSS JOIN range(64) worlds(world)
WHERE presence[world+1]>0
)
SELECT cs1.world AS world, cs1.product_name,
       cs1.store_name,
       cs1.store_zip,
       cs1.b_street_number,
       cs1.b_street_name,
       cs1.b_city,
       cs1.b_zip,
       cs1.c_street_number,
       cs1.c_street_name,
       cs1.c_city,
       cs1.c_zip,
       cs1.syear cs1syear,
       cs1.cnt cs1cnt,
       cs1.s1 AS s11,
       cs1.s2 AS s21,
       cs1.s3 AS s31,
       cs2.s1 AS s12,
       cs2.s2 AS s22,
       cs2.s3 AS s32,
       cs2.syear,
       cs2.cnt
FROM cross_sales cs1,
     cross_sales cs2
WHERE cs1.world=cs2.world AND  cs1.item_sk=cs2.item_sk
  AND cs1.syear = 1999
  AND cs2.syear = 1999 + 1
  AND cs2.cnt <= cs1.cnt
  AND cs1.store_name = cs2.store_name
  AND cs1.store_zip = cs2.store_zip
ORDER BY cs1.world, cs1.product_name,
         cs1.store_name,
         cs2.cnt,
         cs1.s1,
         cs2.s1;
