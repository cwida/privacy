# Query Rewriting

The privacy extension optimizer hook transforms standard SQL aggregate plans into privacy-preserving equivalents. It operates as a **pre-optimizer**, running before DuckDB's built-in optimizers (join ordering, filter pushdown, column lifetime, compressed materialization), so the transformed plan benefits from all standard optimizations automatically.

The active mode (`privacy_mode`) determines which compiler runs:
- **`pac`** (default): Proceeds in two phases matching Algorithm 1 in the PAC paper — top-down PU join insertion + hash derivation, then bottom-up aggregate replacement and categorical rewriting.
- **`dp_elastic`**: Extracts the FK join chain, computes elastic sensitivity, clips SUM inputs per-PU, injects Laplace noise.

## Query Classification

When a query is issued, the compatibility checker traverses the logical plan and classifies it into one of three categories:

### Inconspicuous Queries
Queries that do not reference a PU or a PAC-linked table. These execute unmodified.

### Rejected Queries
Queries that reference privacy-relevant tables but cannot be safely privatized. Rejected when:
- A protected column is returned directly without aggregation (e.g., `SELECT name FROM customer`)
- Protected columns appear in `GROUP BY` keys (would leak individual values)
- The query contains **window functions** (`OVER`)
- The query contains **recursive CTEs** (`WITH RECURSIVE`)
- The query contains disallowed joins: `EXCEPT`, `INTERSECT` (but `UNION`/`CROSS_PRODUCT` are allowed)
- The query has **no allowed aggregation** (`SUM`, `COUNT`, `AVG`, `MIN`, `MAX`)
- Multiple protected tables are joined on columns that are **not** exact PAC LINKs
- The FK path from scanned tables to PU is **cyclic**

### Rewritable Queries
All other queries that reference a PU (directly or indirectly via FK path) and contain at least one allowed aggregation. These are compiled by the bitslice compiler.

## Bitslice Compilation (Phase 1)

The bitslice compiler derives a hash expression for each aggregate and replaces it with a PAC function. This is a single unified approach with a preparatory step: if the PU table is not already part of the query, the compiler first adds FK joins to make it reachable.

### Step 1: Ensure PU Reachability

When the query does not scan the PU table directly (e.g., `SELECT SUM(l_quantity) FROM lineitem`):

1. **Follow FK path** from scanned tables to PU (e.g., `lineitem -> orders -> customer`)
2. **Identify missing tables** in the path that need to be joined
3. **Join elimination**: if `priv_join_elimination=true`, skip joining the PU table itself when only FK columns are needed (e.g., join only `orders`, not `customer`, because `orders.o_custkey` is the FK)
4. **For each instance** of the connecting table:
   - Create fresh `LogicalGet` nodes for missing tables
   - Build join chain with appropriate FK conditions
   - Replace the connecting table's node with the join chain

### Step 2: Hash Derivation and Aggregate Replacement

Once the PU (or its FK proxy) is reachable from every aggregate:

1. **Find all aggregates** that have the PU table (or FK-linked table) in their subtree
2. **For each PU table**, compute a hash from the PU's key columns or FK columns:
   - Single PRIVACY_KEY: `priv_hash(hash(key_col))`
   - Composite PRIVACY_KEY: `priv_hash(hash(key1) XOR hash(key2) XOR ...)` — XOR combines multiple column hashes into one, then priv_hash repairs it to 32 bits
   - FK proxy: `priv_hash(hash(fk_col))` (e.g., `priv_hash(hash(o_custkey))`)
3. **Insert a projection** above the hash source scan that computes the hash as an extra column
4. **Propagate** the hash column through all intermediate operators (projections, joins, filters) between the hash source and the aggregate
5. **Replace** each standard aggregate with its PAC equivalent: `SUM(x)` becomes `priv_noised_sum(hash, x)`, `COUNT(*)` becomes `priv_noised_count(hash)`, etc.
6. For **multiple PUs**, AND all hashes together: `hash1 AND hash2` — a tuple is in sub-sample j only if ALL its PUs have bit j set

> **PAC aggregate naming:** base functions (`priv_count`, `priv_sum`, ...) return `LIST<T>` — 64 per-sample counters. `priv_noised(list<T>):T` applies noise and returns a scalar. The `priv_noised_<aggr>()` variants (e.g., `priv_noised_count`) are fused shortcuts: `priv_noised_count(hash)` = `priv_noised(priv_count(hash))`. Similarly, `priv_select` and `priv_filter` consume counter lists for categorical queries.

### Example: Direct PU scan
```sql
-- Original
SELECT COUNT(*) FROM customer WHERE c_mktsegment = 'BUILDING'
-- Rewritten (conceptual)
SELECT priv_noised_count(priv_hash(hash(c_custkey)))
FROM customer WHERE c_mktsegment = 'BUILDING'
```

### Example: FK chain (PU not in query)
```sql
-- Original
SELECT l_returnflag, SUM(l_quantity) FROM lineitem
WHERE l_shipdate <= '1998-09-02' GROUP BY l_returnflag
-- Step 1: join orders to reach PU FK column
-- Step 2: hash o_custkey, replace SUM with priv_noised_sum
SELECT l_returnflag, priv_noised_sum(priv_hash(hash(o_custkey)), l_quantity)
FROM lineitem JOIN orders ON l_orderkey = o_orderkey
WHERE l_shipdate <= '1998-09-02' GROUP BY l_returnflag
```

## Uncorrelated Subqueries

Uncorrelated subqueries (e.g., `WHERE x > (SELECT AVG(y) FROM t)`) are handled by DuckDB as a `SINGLE` join (returns exactly one row). The PAC compiler:

1. Finds **all aggregates** in the plan (including those inside the subquery)
2. Filters to **target aggregates** — only those that have PU/FK-linked tables in their direct subtree (not through nested aggregates)
3. Inserts hash projections and transforms each aggregate independently
4. The outer query's aggregate and the subquery's aggregate each get their own hash

The subquery's aggregate result feeds into the outer query's filter/comparison. When the subquery contains a PAC aggregate and the outer query doesn't aggregate (categorical query), the categorical rewriter handles it (see below).

## Correlated Subqueries (DELIM_JOIN)

Correlated subqueries (e.g., TPC-H Q17: `WHERE l1.quantity < (SELECT AVG(l2.quantity) FROM lineitem l2 WHERE l2.partkey = l1.partkey)`) are compiled by DuckDB into `DELIM_JOIN` operators. The PAC compiler handles these specially:

1. **Detect** when the same connecting table appears in both outer and inner query (e.g., `lineitem` appears twice)
2. **Find all instances** of the connecting table in the plan tree
3. **Add independent join chains** to each instance (each gets its own `orders` join)
4. **DELIM_JOIN propagation**: DuckDB compiles correlated subqueries into DELIM_JOIN operators, which have a "duplicate eliminated columns" mechanism for passing values from the outer query into the inner subquery. When the hash source is in the outer query but the target aggregate is in the subquery branch, the compiler adds the hash column to the DELIM_JOIN's duplicate-eliminated columns and corresponding DELIM_GET nodes, so the inner aggregate can access it.
5. **Accessibility checks**: verify FK table columns aren't blocked by SEMI/ANTI/MARK joins (which don't pass right-side columns). If blocked, find an accessible alternative or add a fresh join.

### SEMI/ANTI Join Handling
When the FK table is on the right side of a SEMI/ANTI join (e.g., `WHERE EXISTS (SELECT ... FROM orders ...)`), its columns are inaccessible to the outer aggregate. The compiler:
- Searches for an accessible FK table instance in the outer query
- If none found, looks for an alternative accessible table with an FK to the blocked table and adds a new join

## CTEs (Common Table Expressions)

The compiler supports **materialized CTEs** (`WITH ... AS MATERIALIZED`):

1. **Build CTE table map**: maps each CTE's index to the set of base tables it transitively references (including through nested CTE refs). This is resolved once at the start of compilation.
2. **CTE-aware subtree checks**: when checking if a table is reachable from an operator, the compiler follows CTE_REF nodes through the map to check if the referenced CTE transitively contains the target table.
3. **Hash projection above CTE_SCAN**: when the PU table is accessed through a CTE, the compiler can't modify the CTE definition (it's shared across all scan sites). Instead, it inserts a hash projection above the CTE_SCAN node that reads the PU key columns from the CTE's output and computes the hash there.
4. **CTE definition propagation**: the CTE definition must expose the PU key columns in its output for the hash to be computable on the scan side.

Non-materialized CTEs are inlined by DuckDB before the PAC optimizer runs, so they appear as regular subquery trees.

## Inner + Outer Aggregates (Nested Aggregation)

The compiler handles the pattern where an inner aggregate groups by PU key and an outer aggregate aggregates over those results (e.g., TPC-H Q13: inner counts orders per customer, outer counts how many customers have each order count):

1. **Detect PU-key grouping**: when filtering which aggregates to transform, the compiler checks if an inner aggregate's GROUP BY keys contain the PU's PRIVACY_KEY columns or PRIVACY_LINK columns referencing a PU. This is done by inspecting the column bindings in the aggregate's group expressions and tracing them back to their source table scans.
2. When detected, the **inner aggregate is skipped** (it's already partitioned by PU key, so no noise needed there — each group corresponds to a single PU)
3. The **outer aggregate is noised** instead, using the inner aggregate's PU key group column as the hash input. The compiler locates the inner aggregate, finds the column binding for the PU key in its output, and uses that as the hash source for the outer aggregate.
4. This means the hash for the outer aggregate comes from the inner aggregate's GROUP BY output rather than from a table scan — the inner aggregate preserves PU identity in its grouping columns.

### Example: TPC-H Q13 Pattern
```sql
-- Inner aggregate groups by c_custkey (PU key) - NOT noised
-- Outer aggregate counts over those groups - IS noised
SELECT c_count, COUNT(*) AS custdist FROM (
    SELECT c_custkey, COUNT(o_orderkey) AS c_count
    FROM customer LEFT JOIN orders ON ...
    GROUP BY c_custkey  -- groups by PU key
) GROUP BY c_count
```

## Top-K Queries

Top-K queries (`ORDER BY agg LIMIT k`) get special treatment via a dedicated post-optimizer rule:

**Problem**: In the default plan, PAC noise is applied at the aggregate level (below TopN). TopN then operates on noisy values, potentially selecting wrong groups.

**Solution** (`pac_pushdown_topk=true`):
1. Convert PAC aggregates to `_counters` variants (return all 64 counter values as `LIST<FLOAT>`)
2. Insert a **mean projection** (`priv_mean(counters)`) for ordering — gives the true aggregate mean
3. **TopN** selects top-k groups based on the true mean
4. Insert a **noised projection** (`priv_noised(counters)`) above TopN — applies noise only to selected rows, cast back to original type

Two paths depending on plan structure:
- **Path A** (intermediate projections, e.g., string decompress): preserves intermediate projections, adds `priv_mean` passthrough columns
- **Path B** (TopN directly above Aggregate): simpler insertion of MeanProj and NoisedProj

**Superset expansion** (`pac_topk_expansion`): select `ceil(c * K)` candidates with the inner TopN, then a final TopN limits to the original K after noising.

## DISTINCT Aggregates

**Aggregate DISTINCT** (e.g., `COUNT(DISTINCT col)`) is rewritten:

- `COUNT(DISTINCT x)` is handled via pre-aggregation: the compiler inserts a `GROUP BY x` with `bit_or(key_hash)` before the standard `priv_noised_count` aggregate
- `SUM(DISTINCT x)` similarly uses `GROUP BY x` with `bit_or(key_hash)` before `priv_noised_sum`

The compiler detects when an aggregate expression is marked as distinct and inserts the pre-aggregation step automatically.

## Categorical Queries (Scalar and Correlated Subqueries)

Categorical queries arise when a (scalar or correlated) subquery produces a PAC aggregate result that is used outside of a direct aggregation context — typically in projection expressions or filter predicates. The categorical rewriter (Phase 2) handles these by converting the inner PAC aggregate to its `_counters` variant (returning all 64 per-sample values as `LIST<DOUBLE>`) and then applying one of three terminal wrappers depending on how the result is consumed.

### Case 1: `priv_noised` — Projection Expressions

When a PAC aggregate result appears in a **projection expression** (arithmetic over one or more aggregates), the rewriter builds a `list_transform` lambda that evaluates the expression across all 64 possible worlds, then reduces to a scalar with `priv_noised`.

For expressions involving multiple aggregates, `list_zip` combines the counter lists so the lambda can access all values per world.

**TPC-H Q08** illustrates this: the query computes `SUM(CASE nation='BRAZIL' THEN volume ELSE 0 END) / SUM(volume)` — a ratio of two aggregates:

```sql
-- Q08: priv_noised wraps a projection expression over two aggregates
SELECT o_year,
       CAST(priv_noised(
              list_transform(
                list_zip(
                  list_transform(priv_sum(pac_pu, brazil_volume),
                                 lambda y: CAST(y AS DECIMAL(18,2))),
                  list_transform(priv_sum(pac_pu, volume),
                                 lambda y: CAST(y AS DECIMAL(18,2)))),
                lambda x: CAST(x[1] / x[2] AS FLOAT))) AS FLOAT) AS mkt_share
FROM ...
GROUP BY o_year
```

The inner `list_transform` calls cast counters back to their original types; the outer lambda computes the division for each of the 64 worlds; `priv_noised` then reduces the 64-element list to a single noised scalar.

### Case 2: `priv_filter` — Filters on Non-Sensitive Tuples

When a PAC aggregate result appears in a **filter predicate** and the tuples being filtered are **not themselves subject to a PAC aggregate** (i.e., no sensitive aggregation sits above the filter), the rewriter uses `priv_filter`. This evaluates the comparison across all 64 worlds and makes a probabilistic filtering decision (returns true with probability `popcount(mask)/64`).

The rewriter attempts algebraic simplification to emit specialized variants (`priv_filter_gt`, `priv_filter_lt`, etc.) that avoid the lambda overhead.

**TPC-H Q20** illustrates this: the query filters `partsupp` rows where `ps_availqty > 2 * (SELECT SUM(l_quantity) ...)` — the filtered tuples (suppliers) are not themselves aggregated by PAC:

```sql
-- Q20: priv_filter wraps a comparison against a subquery aggregate
SELECT s_name, s_address
  FROM supplier JOIN nation ON ...
 WHERE s_suppkey IN (
    SELECT ps_suppkey FROM partsupp
     WHERE ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'forest%')
       AND priv_filter_gt(ps_availqty * 2,
             (SELECT priv_sum(priv_hash(hash(o_custkey)), l_quantity)
                FROM lineitem JOIN orders ON l_orderkey = o_orderkey
               WHERE l_partkey = ps_partkey AND l_suppkey = ps_suppkey
                 AND l_shipdate >= DATE '1994-01-01'
                 AND l_shipdate < DATE '1995-01-01')))
```

`priv_filter_gt` compares the non-sensitive value `ps_availqty * 2` against each of the 64 counter values of `priv_sum` and returns a UBIGINT mask; the row passes if the mask is non-zero (majority vote).

### Case 3: `priv_select` — Filters With a Sensitive Aggregation Above

When a PAC aggregate result appears in a **filter predicate** and a **PAC aggregate sits above** the filter (i.e., the filtered tuples feed into a sensitive aggregation), `priv_filter` is insufficient — the outer aggregate must know *which* of the 64 worlds each tuple belongs to. The rewriter uses `priv_select`, which AND's the 64-bit filter mask with the outer hash, creating a new hash that encodes both the sub-sampling and the categorical decision.

The rewriter emits specialized variants (`priv_select_lt`, `priv_select_gt`, etc.) when possible.

**TPC-H Q17** illustrates this: the query computes `SUM(l_extendedprice) / 7.0` over lineitems where `l_quantity < 0.2 * AVG(l_quantity)` — the filter depends on a PAC aggregate (inner AVG), and the filtered result feeds into another PAC aggregate (outer SUM):

```sql
-- Q17: priv_select wraps a filter that feeds into an outer PAC aggregate
SELECT priv_noised_sum(pac_pu, l_extendedprice) / 7.0 AS avg_yearly
  FROM (SELECT priv_select_lt(
                 priv_hash(hash(o_custkey)),
                 lineitem.l_quantity * 5,
                 (SELECT priv_div(
                           priv_sum(priv_hash(hash(o_sub.o_custkey)), l_sub.l_quantity),
                           priv_count(priv_hash(hash(o_sub.o_custkey)), l_sub.l_quantity))
                    FROM lineitem AS l_sub JOIN orders AS o_sub ON ...
                   WHERE l_sub.l_partkey = part.p_partkey)) AS pac_pu,
               l_extendedprice
          FROM lineitem JOIN part ON ... JOIN orders ON ...
         WHERE part.p_brand = 'Brand#23' AND part.p_container = 'MED BOX')
 WHERE pac_pu <> 0
```

`priv_select_lt` takes the outer hash, the non-sensitive comparand (`l_quantity * 5`), and the inner aggregate's counter list. It returns a new UBIGINT hash where bit j is set only if both the original sub-sample includes the tuple AND the comparison holds in world j. The outer `priv_noised_sum` then aggregates using this combined hash. The `WHERE pac_pu <> 0` applies majority-vote filtering.
