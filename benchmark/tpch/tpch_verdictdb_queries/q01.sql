-- WIP rebuttal benchmark: hardcoded VerdictDB sigmod18 subsampling rewrite.
WITH partition_estimates AS (
    SELECT vc.verdict_vpart, l_returnflag, l_linestatus,
           count(*) AS __vpsize,
           sum(l_quantity / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_returnflag, l_linestatus) AS sum_qty,
           sum(l_extendedprice / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_returnflag, l_linestatus) AS sum_base_price,
           sum((l_extendedprice * (1 - l_discount)) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_returnflag, l_linestatus) AS sum_disc_price,
           sum((l_extendedprice * (1 - l_discount) * (1 + l_tax)) / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_returnflag, l_linestatus) AS sum_charge,
           sum(l_quantity / vc.verdict_vprob) / sum(1.0 / vc.verdict_vprob) AS avg_qty,
           sum(l_extendedprice / vc.verdict_vprob) / sum(1.0 / vc.verdict_vprob) AS avg_price,
           sum(l_discount / vc.verdict_vprob) / sum(1.0 / vc.verdict_vprob) AS avg_disc,
           sum(1.0 / vc.verdict_vprob) / count(*)
               * sum(count(*)) OVER (PARTITION BY l_returnflag, l_linestatus) AS count_order
    FROM benchmark_verdict_customer vc
    JOIN orders o ON vc.c_custkey = o.o_custkey
    JOIN lineitem l ON l.l_orderkey = o.o_orderkey
    WHERE l_shipdate <= DATE '1998-09-02'
    GROUP BY vc.verdict_vpart, l_returnflag, l_linestatus
)
SELECT l_returnflag, l_linestatus,
       sum(sum_qty * __vpsize) / sum(__vpsize) AS sum_qty,
       stddev_samp(sum_qty) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS sum_qty_error,
       sum(sum_base_price * __vpsize) / sum(__vpsize) AS sum_base_price,
       stddev_samp(sum_base_price) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96
           AS sum_base_price_error,
       sum(sum_disc_price * __vpsize) / sum(__vpsize) AS sum_disc_price,
       stddev_samp(sum_disc_price) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96
           AS sum_disc_price_error,
       sum(sum_charge * __vpsize) / sum(__vpsize) AS sum_charge,
       stddev_samp(sum_charge) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS sum_charge_error,
       sum(avg_qty * __vpsize) / sum(__vpsize) AS avg_qty,
       stddev_samp(avg_qty) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS avg_qty_error,
       sum(avg_price * __vpsize) / sum(__vpsize) AS avg_price,
       stddev_samp(avg_price) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS avg_price_error,
       sum(avg_disc * __vpsize) / sum(__vpsize) AS avg_disc,
       stddev_samp(avg_disc) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS avg_disc_error,
       round(sum(count_order * __vpsize) / sum(__vpsize)) AS count_order,
       stddev_samp(count_order) * sqrt(avg(__vpsize)) / sqrt(sum(__vpsize)) * 1.96 AS count_order_error
FROM partition_estimates
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
