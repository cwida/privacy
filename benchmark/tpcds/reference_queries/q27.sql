-- Explicit grouping sets reproduce all three original UNION ALL branches, including empty grand totals.
WITH input AS (
SELECT priv_hash(hash(ss_customer_sk)) AS membership, i_item_id,
          s_state,
          0 AS g_state,
          ss_quantity agg1,
          ss_list_price agg2,
          ss_coupon_amt agg3,
          ss_sales_price agg4
   FROM main.store_sales,
        main.customer_demographics,
        main.date_dim,
        main.store,
        main.item, (SELECT c_customer_sk AS asa_key FROM main.customer) asa_owner
WHERE ss_customer_sk=asa_owner.asa_key AND  ss_sold_date_sk = d_date_sk
     AND ss_item_sk = i_item_sk
     AND ss_store_sk = s_store_sk
     AND ss_cdemo_sk = cd_demo_sk
     AND cd_gender = 'M'
     AND cd_marital_status = 'S'
     AND cd_education_status = 'College'
     AND d_year = 2002
     AND s_state = 'TN'
), stats AS (SELECT i_item_id, s_state, grouping(s_state) AS g_state, grouping(i_item_id) AS grand_total, tpcds_avg(membership,agg1) AS agg1, tpcds_avg(membership,agg2) AS agg2, tpcds_avg(membership,agg3) AS agg3, tpcds_avg(membership,agg4) AS agg4, tpcds_count(membership,1) AS presence FROM input GROUP BY GROUPING SETS ((i_item_id,s_state),(i_item_id),())), answers AS (SELECT world, i_item_id, s_state, g_state, agg1[world+1] AS agg1, agg2[world+1] AS agg2, agg3[world+1] AS agg3, agg4[world+1] AS agg4 FROM stats CROSS JOIN range(64) worlds(world) WHERE presence[world+1]>0 OR grand_total=1)
SELECT * FROM answers QUALIFY row_number() OVER (PARTITION BY world ORDER BY i_item_id NULLS FIRST,s_state NULLS FIRST)<=100 ORDER BY world,i_item_id NULLS FIRST,s_state NULLS FIRST;
