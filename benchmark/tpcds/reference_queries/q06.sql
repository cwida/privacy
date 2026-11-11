-- Explicit AS query; tpcds_* macros normalize the public aggregate scaling and NULL semantics.
WITH stats AS (
 SELECT a.ca_state AS state,
        tpcds_count((priv_hash(hash(s.ss_customer_sk))), 1) AS cnt,
        tpcds_count((priv_hash(hash(s.ss_customer_sk))), 1) AS presence
 FROM main.customer_address a ,
     main.customer c ,
     main.store_sales s ,
     main.date_dim d ,
     main.item i,
     (SELECT c_customer_sk AS asa_key0 FROM main.customer) asa_owner0
WHERE s.ss_customer_sk = asa_owner0.asa_key0
  AND a.ca_address_sk = c.c_current_addr_sk
  AND c.c_customer_sk = s.ss_customer_sk
  AND s.ss_sold_date_sk = d.d_date_sk
  AND s.ss_item_sk = i.i_item_sk
  AND d.d_month_seq =
    (SELECT DISTINCT (d_month_seq)
     FROM main.date_dim
     WHERE d_year = 2001
       AND d_moy = 1 )
  AND i.i_current_price > 1.2 *
    (SELECT avg(j.i_current_price)
     FROM main.item j
     WHERE j.i_category = i.i_category)
GROUP BY a.ca_state
), answers AS (
 SELECT world, state,
        cnt[world+1] AS cnt
 FROM stats CROSS JOIN range(64) worlds(world)
 WHERE presence[world+1] > 0
)
SELECT * FROM answers
WHERE cnt >= 10
QUALIFY row_number() OVER (PARTITION BY world ORDER BY cnt NULLS FIRST, state NULLS FIRST) <= 100
ORDER BY world, cnt NULLS FIRST, state NULLS FIRST;
