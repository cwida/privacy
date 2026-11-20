-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- COUNT(DISTINCT) unions masks per order before counting; SUMs retain their own source scan.
WITH asa_input AS NOT MATERIALIZED (SELECT cs_order_number, cs_ext_ship_cost, cs_net_profit, priv_hash(hash(cs1.cs_bill_customer_sk)) AS membership FROM catalog_sales cs1,
     date_dim,
     customer_address,
     call_center
WHERE d_date BETWEEN '2002-02-01' AND cast('2002-04-02' AS date)
  AND cs1.cs_ship_date_sk = d_date_sk
  AND cs1.cs_ship_addr_sk = ca_address_sk
  AND ca_state = 'GA'
  AND cs1.cs_call_center_sk = cc_call_center_sk
  AND cc_county = 'Williamson County'
  AND EXISTS
    (SELECT *
     FROM catalog_sales cs2
     WHERE cs1.cs_order_number = cs2.cs_order_number
       AND cs1.cs_warehouse_sk <> cs2.cs_warehouse_sk)
  AND NOT EXISTS
    (SELECT *
     FROM catalog_returns cr1
     WHERE cs1.cs_order_number = cr1.cr_order_number)
),
asa_distinct AS NOT MATERIALIZED (SELECT cs_order_number, bit_or(membership) FILTER (WHERE cs_order_number IS NOT NULL) AS membership FROM asa_input GROUP BY cs_order_number)
SELECT d.n AS "order count", s.shipping AS "total shipping cost", s.profit AS "total net profit" FROM (SELECT as_noised_count(membership, 1::BIGINT) AS n FROM asa_distinct) d CROSS JOIN (SELECT as_noised_sum(membership, cs_ext_ship_cost) AS shipping, as_noised_sum(membership, cs_net_profit) AS profit FROM asa_input) s ORDER BY "order count" LIMIT 100;
