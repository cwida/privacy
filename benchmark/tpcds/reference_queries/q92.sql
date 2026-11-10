-- Correlated aggregate predicates update the mask before the outer AS SUM.
WITH thresholds AS (
 SELECT ws_item_sk, tpcds_avg(priv_hash(hash(ws_bill_customer_sk)),ws_ext_discount_amt) AS averages
 FROM main.web_sales JOIN main.customer ON ws_bill_customer_sk=c_customer_sk JOIN main.date_dim ON ws_sold_date_sk=d_date_sk
 WHERE d_date BETWEEN DATE '2000-01-27' AND DATE '2000-04-26' GROUP BY ws_item_sk
), filtered AS (
 SELECT s.ws_ext_discount_amt AS discount,
        priv_select(priv_hash(hash(ws_bill_customer_sk)),list_transform(averages, lambda average: s.ws_ext_discount_amt>1.3*average)) AS membership
 FROM main.web_sales s JOIN main.customer ON ws_bill_customer_sk=c_customer_sk
 JOIN main.item ON s.ws_item_sk=i_item_sk JOIN main.date_dim ON s.ws_sold_date_sk=d_date_sk
 JOIN thresholds t ON s.ws_item_sk=t.ws_item_sk
 WHERE i_manufact_id=350 AND d_date BETWEEN DATE '2000-01-27' AND DATE '2000-04-26'
), stats AS (SELECT tpcds_sum(membership,discount) AS total FROM filtered)
SELECT world,total[world+1] AS excess_discount_amount FROM stats CROSS JOIN range(64) worlds(world) ORDER BY world;
