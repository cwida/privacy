-- Filterless / CROWD mechanism, written out by hand in SQL.
--
-- Shows what the compiler would generate, and where the boundary sits between the
-- one-time metadata pass (no filters, frozen) and the per-query path (filtered).
-- Runnable as-is: duckdb -init /dev/null < attacks/filterless_example.sql
--
-- Measure = SUM(l_extendedprice), PU = customer, grouping = order month.
-- Bin factor 2, CROWD support 350.

ATTACH 'tpch_sf1.db' AS tpch (READ_ONLY);

-- ===========================================================================
-- METADATA PASS — once per (measure, grouping). No WHERE clause anywhere.
-- ===========================================================================

-- Full-domain contribution of each PU to each group. This is the relation Peter
-- would materialize and maintain with IVM.
CREATE OR REPLACE TABLE pu_contrib AS
SELECT o_custkey                      AS pu,
       strftime(o_orderdate, '%Y-%m') AS g,
       sum(l_extendedprice)           AS a
FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey
GROUP BY 1, 2;

-- Per-group bound: bin the contributions by powers of 2, keep the highest bin that
-- at least 350 distinct PUs land in, and take the top of that bin. A lone outlier
-- sits in a bin of its own and is ignored. Groups with no supported bin never
-- appear here, which is also the released group universe.
CREATE OR REPLACE TABLE group_bound AS
WITH bins AS (
    SELECT g, cast(floor(log2(a)) AS INTEGER) AS bin, count(*) AS support
    FROM pu_contrib
    WHERE a > 0
    GROUP BY 1, 2
)
SELECT g, pow(2, max(bin) + 1) AS b_g
FROM bins
WHERE support >= 350
GROUP BY g;

-- Crowd total: the same rule, applied one level up to each PU's clipped total
-- across groups. One scalar. This is the noise calibration, and it is what
-- replaces "the largest single user's total".
CREATE OR REPLACE TABLE crowd_norm AS
WITH norms AS (
    SELECT pu, sum(least(a, b_g)) AS n_u
    FROM pu_contrib JOIN group_bound USING (g)
    GROUP BY pu
), bins AS (
    SELECT cast(floor(log2(n_u)) AS INTEGER) AS bin, count(*) AS support
    FROM norms
    WHERE n_u > 0
    GROUP BY 1
)
SELECT pow(2, max(bin) + 1) AS d_s
FROM bins
WHERE support >= 350;

SELECT (SELECT count(*) FROM group_bound)   AS groups_released,
       (SELECT median(b_g) FROM group_bound) AS median_group_bound,
       (SELECT d_s FROM crowd_norm)          AS crowd_total;

-- ===========================================================================
-- QUERY TIME — the analyst's query, rewritten. The filter appears only here.
-- ===========================================================================
--
-- Original:
--   SELECT strftime(o_orderdate, '%Y-%m'), sum(l_extendedprice)
--   FROM lineitem JOIN orders ON o_orderkey = l_orderkey
--   WHERE l_shipmode IN ('AIR', 'REG AIR')
--   GROUP BY 1;

WITH per_pu AS (
    -- Per-PU pre-aggregation, with the analyst's filter.
    SELECT o_custkey                      AS pu,
           strftime(o_orderdate, '%Y-%m') AS g,
           sum(l_extendedprice)           AS t
    FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey
    WHERE l_shipmode IN ('AIR', 'REG AIR')
    GROUP BY 1, 2
), clipped AS (
    -- Clip to the frozen per-group bound. Join on the grouping key, exactly as
    -- Peter proposed; groups absent from group_bound drop out here.
    SELECT p.pu, p.g, least(p.t, gb.b_g) AS c
    FROM per_pu p JOIN group_bound gb USING (g)
), scaled AS (
    -- Scale each PU down so its total across groups fits under the crowd total.
    -- One window aggregate; no extra join. After this, no PU can move the result
    -- vector by more than the crowd total, so that IS the sensitivity.
    SELECT pu, g,
           c * least(1.0, (SELECT d_s FROM crowd_norm) / sum(c) OVER (PARTITION BY pu)) AS c
    FROM clipped
), noise AS (
    -- Laplace(crowd_total / epsilon) per group, epsilon = 1.
    SELECT g, sum(c) AS clipped_sum,
           (SELECT d_s FROM crowd_norm) / 1.0 AS scale,
           random() - 0.5                     AS u
    FROM scaled
    GROUP BY g
)
SELECT g,
       clipped_sum - scale * sign(u) * ln(1 - 2 * abs(u)) AS released
FROM noise
ORDER BY g
LIMIT 10;

-- ===========================================================================
-- Sanity check: how much did the mechanism cost, per group?
-- ===========================================================================
WITH truth AS (
    SELECT strftime(o_orderdate, '%Y-%m') AS g, sum(l_extendedprice) AS true_sum
    FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey
    WHERE l_shipmode IN ('AIR', 'REG AIR')
    GROUP BY 1
), per_pu AS (
    SELECT o_custkey AS pu, strftime(o_orderdate, '%Y-%m') AS g, sum(l_extendedprice) AS t
    FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey
    WHERE l_shipmode IN ('AIR', 'REG AIR')
    GROUP BY 1, 2
), scaled AS (
    SELECT p.pu, p.g,
           least(p.t, gb.b_g) * least(1.0, (SELECT d_s FROM crowd_norm)
                                          / sum(least(p.t, gb.b_g)) OVER (PARTITION BY p.pu)) AS c
    FROM per_pu p JOIN group_bound gb USING (g)
)
SELECT round(100 * median(abs(s.clipped_sum - t.true_sum) / t.true_sum), 2) AS median_clip_loss_pct,
       round(100 * median((SELECT d_s FROM crowd_norm) / t.true_sum), 2)     AS median_noise_pct
FROM (SELECT g, sum(c) AS clipped_sum FROM scaled GROUP BY g) s
JOIN truth t USING (g);
