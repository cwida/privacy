-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- Each bucket has independent scalar aggregates and a mask-union DISTINCT-count branch.
WITH b1_input AS NOT MATERIALIZED (SELECT ss_list_price, priv_hash(hash(ss_customer_sk)) AS membership FROM store_sales
   WHERE ss_quantity BETWEEN 0 AND 5
     AND (ss_list_price BETWEEN 8 AND 8+10
          OR ss_coupon_amt BETWEEN 459 AND 459+1000
          OR ss_wholesale_cost BETWEEN 57 AND 57+20)),
b1_unique AS NOT MATERIALIZED (SELECT ss_list_price, bit_or(membership) FILTER (WHERE ss_list_price IS NOT NULL) AS membership FROM b1_input GROUP BY ss_list_price),
b1 AS NOT MATERIALIZED (SELECT CAST(as_noised_sum(membership,ss_list_price) AS DOUBLE)/CAST(as_noised_count(membership,ss_list_price) AS DOUBLE) AS B1_LP, as_noised_count(membership,ss_list_price) AS B1_CNT, (SELECT as_noised_count(membership,1::BIGINT) FROM b1_unique) AS B1_CNTD FROM b1_input),
b2_input AS NOT MATERIALIZED (SELECT ss_list_price, priv_hash(hash(ss_customer_sk)) AS membership FROM store_sales
   WHERE ss_quantity BETWEEN 6 AND 10
     AND (ss_list_price BETWEEN 90 AND 90+10
          OR ss_coupon_amt BETWEEN 2323 AND 2323+1000
          OR ss_wholesale_cost BETWEEN 31 AND 31+20)),
b2_unique AS NOT MATERIALIZED (SELECT ss_list_price, bit_or(membership) FILTER (WHERE ss_list_price IS NOT NULL) AS membership FROM b2_input GROUP BY ss_list_price),
b2 AS NOT MATERIALIZED (SELECT CAST(as_noised_sum(membership,ss_list_price) AS DOUBLE)/CAST(as_noised_count(membership,ss_list_price) AS DOUBLE) AS B2_LP, as_noised_count(membership,ss_list_price) AS B2_CNT, (SELECT as_noised_count(membership,1::BIGINT) FROM b2_unique) AS B2_CNTD FROM b2_input),
b3_input AS NOT MATERIALIZED (SELECT ss_list_price, priv_hash(hash(ss_customer_sk)) AS membership FROM store_sales
   WHERE ss_quantity BETWEEN 11 AND 15
     AND (ss_list_price BETWEEN 142 AND 142+10
          OR ss_coupon_amt BETWEEN 12214 AND 12214+1000
          OR ss_wholesale_cost BETWEEN 79 AND 79+20)),
b3_unique AS NOT MATERIALIZED (SELECT ss_list_price, bit_or(membership) FILTER (WHERE ss_list_price IS NOT NULL) AS membership FROM b3_input GROUP BY ss_list_price),
b3 AS NOT MATERIALIZED (SELECT CAST(as_noised_sum(membership,ss_list_price) AS DOUBLE)/CAST(as_noised_count(membership,ss_list_price) AS DOUBLE) AS B3_LP, as_noised_count(membership,ss_list_price) AS B3_CNT, (SELECT as_noised_count(membership,1::BIGINT) FROM b3_unique) AS B3_CNTD FROM b3_input),
b4_input AS NOT MATERIALIZED (SELECT ss_list_price, priv_hash(hash(ss_customer_sk)) AS membership FROM store_sales
   WHERE ss_quantity BETWEEN 16 AND 20
     AND (ss_list_price BETWEEN 135 AND 135+10
          OR ss_coupon_amt BETWEEN 6071 AND 6071+1000
          OR ss_wholesale_cost BETWEEN 38 AND 38+20)),
b4_unique AS NOT MATERIALIZED (SELECT ss_list_price, bit_or(membership) FILTER (WHERE ss_list_price IS NOT NULL) AS membership FROM b4_input GROUP BY ss_list_price),
b4 AS NOT MATERIALIZED (SELECT CAST(as_noised_sum(membership,ss_list_price) AS DOUBLE)/CAST(as_noised_count(membership,ss_list_price) AS DOUBLE) AS B4_LP, as_noised_count(membership,ss_list_price) AS B4_CNT, (SELECT as_noised_count(membership,1::BIGINT) FROM b4_unique) AS B4_CNTD FROM b4_input),
b5_input AS NOT MATERIALIZED (SELECT ss_list_price, priv_hash(hash(ss_customer_sk)) AS membership FROM store_sales
   WHERE ss_quantity BETWEEN 21 AND 25
     AND (ss_list_price BETWEEN 122 AND 122+10
          OR ss_coupon_amt BETWEEN 836 AND 836+1000
          OR ss_wholesale_cost BETWEEN 17 AND 17+20)),
b5_unique AS NOT MATERIALIZED (SELECT ss_list_price, bit_or(membership) FILTER (WHERE ss_list_price IS NOT NULL) AS membership FROM b5_input GROUP BY ss_list_price),
b5 AS NOT MATERIALIZED (SELECT CAST(as_noised_sum(membership,ss_list_price) AS DOUBLE)/CAST(as_noised_count(membership,ss_list_price) AS DOUBLE) AS B5_LP, as_noised_count(membership,ss_list_price) AS B5_CNT, (SELECT as_noised_count(membership,1::BIGINT) FROM b5_unique) AS B5_CNTD FROM b5_input),
b6_input AS NOT MATERIALIZED (SELECT ss_list_price, priv_hash(hash(ss_customer_sk)) AS membership FROM store_sales
   WHERE ss_quantity BETWEEN 26 AND 30
     AND (ss_list_price BETWEEN 154 AND 154+10
          OR ss_coupon_amt BETWEEN 7326 AND 7326+1000
          OR ss_wholesale_cost BETWEEN 7 AND 7+20)),
b6_unique AS NOT MATERIALIZED (SELECT ss_list_price, bit_or(membership) FILTER (WHERE ss_list_price IS NOT NULL) AS membership FROM b6_input GROUP BY ss_list_price),
b6 AS NOT MATERIALIZED (SELECT CAST(as_noised_sum(membership,ss_list_price) AS DOUBLE)/CAST(as_noised_count(membership,ss_list_price) AS DOUBLE) AS B6_LP, as_noised_count(membership,ss_list_price) AS B6_CNT, (SELECT as_noised_count(membership,1::BIGINT) FROM b6_unique) AS B6_CNTD FROM b6_input)
SELECT * FROM b1, b2, b3, b4, b5, b6 LIMIT 100;
