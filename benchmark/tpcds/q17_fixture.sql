-- Synthetic nonempty correctness fixture for the unchanged TPC-DS Q17 SQL, not a performance dataset.
-- Run against a fresh database. Empty owned tables allow the standard ownership setup to be reused.
SET priv_rewrite=false;
CREATE TABLE customer AS SELECT i::INTEGER AS c_customer_sk FROM range(128) r(i);
CREATE TABLE store_sales AS
SELECT c_customer_sk AS ss_customer_sk, 1 AS ss_item_sk, c_customer_sk AS ss_ticket_number,
       1 AS ss_sold_date_sk, 1 AS ss_store_sk, c_customer_sk % 10 + 1 AS ss_quantity
FROM customer;
CREATE TABLE store_returns AS
SELECT c_customer_sk AS sr_customer_sk, 1 AS sr_item_sk, c_customer_sk AS sr_ticket_number,
       1 AS sr_returned_date_sk, c_customer_sk % 4 + 1 AS sr_return_quantity
FROM customer;
CREATE TABLE catalog_sales AS
SELECT c_customer_sk AS cs_bill_customer_sk, 1 AS cs_item_sk, 1 AS cs_sold_date_sk,
       c_customer_sk % 13 + 1 AS cs_quantity
FROM customer;
CREATE TABLE catalog_returns(cr_returning_customer_sk INTEGER);
CREATE TABLE web_sales(ws_bill_customer_sk INTEGER);
CREATE TABLE web_returns(wr_returning_customer_sk INTEGER);
CREATE TABLE date_dim AS SELECT 1 AS d_date_sk, '2001Q1' AS d_quarter_name;
CREATE TABLE store AS SELECT 1 AS s_store_sk, 'CA' AS s_state;
CREATE TABLE item AS SELECT 1 AS i_item_sk, 'ITEM1' AS i_item_id, 'test item' AS i_item_desc;
