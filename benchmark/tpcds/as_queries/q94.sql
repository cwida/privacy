-- Handwritten PAC SQL, following the compiled aggregate scopes and scalar output semantics.
-- COUNT(DISTINCT) unions membership by order; EXISTS/NOT EXISTS retain their original SQL semantics.
WITH asa_input AS NOT MATERIALIZED (SELECT ws_order_number, ws_ext_ship_cost, ws_net_profit, priv_hash(hash(ws1.ws_bill_customer_sk)) AS membership FROM web_sales ws1 ,
     date_dim ,
     customer_address ,
     web_site
WHERE d_date BETWEEN '1999-02-01' AND cast('1999-04-02' AS date)
  AND ws1.ws_ship_date_sk = d_date_sk
  AND ws1.ws_ship_addr_sk = ca_address_sk
  AND ca_state = 'IL'
  AND ws1.ws_web_site_sk = web_site_sk
  AND web_company_name = 'pri'
  AND EXISTS
    (SELECT *
     FROM web_sales ws2
     WHERE ws1.ws_order_number = ws2.ws_order_number
       AND ws1.ws_warehouse_sk <> ws2.ws_warehouse_sk)
  AND NOT exists
    (SELECT *
     FROM web_returns wr1
     WHERE ws1.ws_order_number = wr1.wr_order_number)
),
asa_distinct AS NOT MATERIALIZED (SELECT ws_order_number, bit_or(membership) FILTER (WHERE ws_order_number IS NOT NULL) AS membership FROM asa_input GROUP BY ws_order_number)
SELECT d.n AS "order count", s.shipping AS "total shipping cost", s.profit AS "total net profit" FROM (SELECT as_noised_count(membership, 1::BIGINT) AS n FROM asa_distinct) d CROSS JOIN (SELECT as_noised_sum(membership, ws_ext_ship_cost) AS shipping, as_noised_sum(membership, ws_net_profit) AS profit FROM asa_input) s ORDER BY "order count" LIMIT 100;
