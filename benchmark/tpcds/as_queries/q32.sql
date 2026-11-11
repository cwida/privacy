-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- The correlated threshold narrows the tuple mask before the outer stochastic SUM.
SELECT as_noised_sum(priv_select_gt(priv_hash(hash(cs_bill_customer_sk)), CAST(cs_ext_discount_amt AS FLOAT), ( SELECT list_transform(priv_div(as_sum(priv_hash(hash(cs_bill_customer_sk)), CAST(cs_ext_discount_amt AS DOUBLE)), as_count(priv_hash(hash(cs_bill_customer_sk)), cs_ext_discount_amt)), lambda x: CAST(1.3 * x AS FLOAT))
     FROM catalog_sales ,
          date_dim
     WHERE cs_item_sk = i_item_sk
       AND d_date BETWEEN '2000-01-27' AND cast('2000-04-26' AS date)
       AND d_date_sk = cs_sold_date_sk )), cs_ext_discount_amt) AS "excess discount amount"
FROM catalog_sales ,
     item ,
     date_dim
WHERE i_manufact_id = 977
  AND i_item_sk = cs_item_sk
  AND d_date BETWEEN '2000-01-27' AND cast('2000-04-26' AS date)
  AND d_date_sk = cs_sold_date_sk
  AND priv_select_gt(priv_hash(hash(cs_bill_customer_sk)), CAST(cs_ext_discount_amt AS FLOAT), ( SELECT list_transform(priv_div(as_sum(priv_hash(hash(cs_bill_customer_sk)), CAST(cs_ext_discount_amt AS DOUBLE)), as_count(priv_hash(hash(cs_bill_customer_sk)), cs_ext_discount_amt)), lambda x: CAST(1.3 * x AS FLOAT))
     FROM catalog_sales ,
          date_dim
     WHERE cs_item_sk = i_item_sk
       AND d_date BETWEEN '2000-01-27' AND cast('2000-04-26' AS date)
       AND d_date_sk = cs_sold_date_sk )) <> 0
LIMIT 100;
