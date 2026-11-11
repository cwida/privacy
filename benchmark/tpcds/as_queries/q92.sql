-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
SELECT as_noised_sum(priv_select_gt(priv_hash(hash(ws_bill_customer_sk)), CAST(ws_ext_discount_amt AS FLOAT), (SELECT list_transform(priv_div(as_sum(priv_hash(hash(ws_bill_customer_sk)), CAST(ws_ext_discount_amt AS DOUBLE)), as_count(priv_hash(hash(ws_bill_customer_sk)), ws_ext_discount_amt)), lambda x: CAST(1.3 * x AS FLOAT))
     FROM web_sales,
          date_dim
     WHERE ws_item_sk = i_item_sk
       AND d_date BETWEEN '2000-01-27' AND cast('2000-04-26' AS date)
       AND d_date_sk = ws_sold_date_sk )), ws_ext_discount_amt) AS "Excess Discount Amount"
FROM web_sales,
     item,
     date_dim
WHERE i_manufact_id = 350
  AND i_item_sk = ws_item_sk
  AND d_date BETWEEN '2000-01-27' AND cast('2000-04-26' AS date)
  AND d_date_sk = ws_sold_date_sk
  AND priv_select_gt(priv_hash(hash(ws_bill_customer_sk)), CAST(ws_ext_discount_amt AS FLOAT), (SELECT list_transform(priv_div(as_sum(priv_hash(hash(ws_bill_customer_sk)), CAST(ws_ext_discount_amt AS DOUBLE)), as_count(priv_hash(hash(ws_bill_customer_sk)), ws_ext_discount_amt)), lambda x: CAST(1.3 * x AS FLOAT))
     FROM web_sales,
          date_dim
     WHERE ws_item_sk = i_item_sk
       AND d_date BETWEEN '2000-01-27' AND cast('2000-04-26' AS date)
       AND d_date_sk = ws_sold_date_sk )) <> 0
ORDER BY "Excess Discount Amount"
LIMIT 100;
