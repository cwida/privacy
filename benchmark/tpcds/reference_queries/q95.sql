-- Order-level existence masks are combined before AS; DISTINCT unions memberships by order.
WITH warehouses AS (SELECT ws_order_number AS order_number,ws_warehouse_sk AS warehouse,bit_or(priv_hash(hash(ws_bill_customer_sk))) AS membership FROM main.web_sales JOIN main.customer ON ws_bill_customer_sk=c_customer_sk GROUP BY ws_order_number,ws_warehouse_sk),
pair_orders AS (SELECT a.order_number,bit_or(a.membership & b.membership) AS membership FROM warehouses a JOIN warehouses b ON a.order_number=b.order_number AND a.warehouse<>b.warehouse GROUP BY a.order_number),
return_orders AS (SELECT wr_order_number AS order_number,bit_or(priv_hash(hash(wr_returning_customer_sk))) AS membership FROM main.web_returns JOIN main.customer ON wr_returning_customer_sk=c_customer_sk GROUP BY wr_order_number),
input AS (SELECT ws_order_number AS order_number,ws_ext_ship_cost AS shipping,ws_net_profit AS profit,(priv_hash(hash(ws_bill_customer_sk)) & other.membership & coalesce(ret.membership,0::UBIGINT)) AS membership
FROM main.web_sales JOIN main.customer ON ws_bill_customer_sk=c_customer_sk JOIN main.date_dim ON ws_ship_date_sk=d_date_sk JOIN main.customer_address ON ws_ship_addr_sk=ca_address_sk JOIN main.web_site ON ws_web_site_sk=web_site_sk JOIN pair_orders other ON ws_order_number=other.order_number LEFT JOIN return_orders ret ON ws_order_number=ret.order_number WHERE d_date BETWEEN DATE '1999-02-01' AND DATE '1999-04-02' AND ca_state='IL' AND web_company_name='pri'),
totals AS (SELECT tpcds_sum(membership,shipping) AS shipping,tpcds_sum(membership,profit) AS profit FROM input),
unique_orders AS (SELECT order_number,bit_or(membership) AS membership FROM input GROUP BY order_number),
counts AS (SELECT tpcds_count(membership,1) AS n FROM unique_orders)
SELECT world,n[world+1] AS order_count,shipping[world+1] AS total_shipping_cost,profit[world+1] AS total_net_profit FROM totals CROSS JOIN counts CROSS JOIN range(64) worlds(world) ORDER BY world;
