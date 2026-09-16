# Code review: multi-PU ownership bounds prototype

Reviewed 2026-09-04 against Dandan Yuan's draft *Automatic Contribution Bounding under Multi-PU
Provenance* and DP4SQL's schema-level ownership assumptions.

## Conclusion

The current code is a useful exact-data diagnostic for ownership paths of one base table. Its
recurrence correctly computes the draft's conservative `c(R)`, `o(R)`, and `c(R)(o(R)-1)` quantities
for that restricted graph, and the exact co-owner calculation is useful for measuring looseness.

It is not yet the draft's schema-level mechanism and must not be connected to DP noise calibration.
The two main blockers are that it substitutes current-instance frequencies for public schema MMFs
and that it analyzes a base table rather than the final query's pre-aggregation relation.

## Findings

### 1. Blocker: `graph_max_affected_pus` is data-dependent, not `U_schema`

`ValidateLinkAndGetMaxFrequency` scans the private child table and computes its current maximum
frequency. `ComputeNodeBounds` then uses those observed values as the edge multipliers for
`graph_max_affected_pus`.

This only upper-bounds the observed instance. Dandan's draft and DP4SQL require each MMF to be a
public, instance-independent bound valid for every database in the neighboring domain. A neighboring
database can have a larger frequency, so this value cannot calibrate the influence histogram or any
later DP release.

The code and header acknowledge that the result is unsafe, but the output name `graph_max...` is
easy to confuse with the formal schema bound. It should be named `observed_graph_upper_bound` until
a separate schema analysis exists.

Required design change:

- represent declared/enforced MMF metadata separately from observed statistics;
- make the schema analyzer pure over metadata, with no `Connection` or data scans; and
- keep observed maxima in a clearly isolated debug-only comparison path.

Relevant code: `src/compiler/dp_ownership_graph.cpp:251-289`, `292-318`, and `429-443`.

### 2. Blocker: the analyzer does not inspect the SQL query or final relation `T`

The draft propagates `o`, `c`, and MMFs bottom-up through selection, projection, ownership-consistent
key joins, and general equijoins until it reaches the final pre-aggregation relation. The current
function accepts only a fact-table name and traverses that table's `PRIVACY_LINK` metadata.

This misses ownership introduced by query joins. A real CLI check used:

```text
q_left:  one row owned only by Alice, join_key = 7
q_right: one row owned only by Bob,   join_key = 7
query:   q_left JOIN q_right USING (join_key)
```

The diagnostic reports degree 0 for both base tables, while the joined tuple has provenance
`{Alice, Bob}` and therefore degree 1 in the actual pre-aggregation relation.

There is also no invocation from the privacy compiler: the analyzer is called only by the debug
table function. The next implementation layer must operate on DuckDB's logical plan and preserve
attribute lineage and composite join keys.

Relevant code: `src/compiler/dp_ownership_graph.cpp:412-418` and
`src/table_functions/privacy_ownership_bounds_debug.cpp:52-56`.

### 3. Blocker for compiler use: the helper connection sees a different snapshot

`AnalyzeObservedDPOwnershipGraph` opens a new DuckDB `Connection`. It cannot see temporary tables or
the caller's uncommitted changes.

This was reproduced in the CLI:

- temporary ownership tables fail with `Table ... does not exist`; and
- after an uncommitted insert changed the expected co-owner degree from 0 to 1, the function still
  returned 0.

That is acceptable only for a documented committed-state diagnostic. It would be an unsafe
underestimate if reused during compilation of the caller's query. A compiler analysis should use
the logical plan and public metadata; any observed diagnostic must explicitly define its snapshot.

Relevant code: `src/compiler/dp_ownership_graph.cpp:420-443`.

### 4. High: current-data validation cannot establish schema assumptions

The function scans tables to check non-null keys, uniqueness, non-null links, and absence of orphaned
references. These checks establish properties of one snapshot only. They do not make those
properties valid over the neighboring database domain.

For the formal schema stage, PK/FK/uniqueness and MMF assumptions must be declared and enforced.
Query-time success versus failure based on private rows would also become an observable side channel
if this validation were placed in the DP query path.

The existing checks remain useful for an unsafe diagnostic, but they should not live in or be reused
by the future schema analyzer.

Relevant code: `src/compiler/dp_ownership_graph.cpp:226-290`.

### 5. High before release: the exact oracle is registered unconditionally

`privacy_ownership_bounds_debug` returns exact maximum frequencies and exact maximum co-owner degree,
with no noise or access guard. It remains callable after setting a formal DP mode and epsilon.

The `_debug` suffix is a warning, not an enforcement boundary. If analysts must be restricted to a DP
transcript, this function must be disabled by default, registered only in a debug build, or protected
by an explicit trusted-administrator capability.

Relevant code: `src/core/privacy_extension.cpp:653` and
`src/table_functions/privacy_ownership_bounds_debug.cpp:91-94`.

### 6. Medium: exact owner enumeration is exponential in the number of DAG paths

`EnumeratePaths` materializes every fact-to-PU path and `ComputeObservedMaxAffectedPus` creates one SQL
branch for each path. A layered diamond ownership DAG can have exponentially many paths even though
the number of tables and edges is small. The resulting owner relation is then self-joined per fact,
adding quadratic work in the number of owners.

For a diagnostic intended to compare realistic schemas, build one memoized owner relation/CTE per
graph node instead of expanding root-to-leaf paths.

Relevant code: `src/compiler/dp_ownership_graph.cpp:321-369` and `397-407`.

### 7. Medium: schema-qualified names are silently discarded

`NormalizeTableName` turns `schema.fact` into `fact`, and generated SQL quotes only the unqualified
name. If a same-named table exists on the helper connection's search path, the diagnostic can inspect
the wrong table.

Until privacy metadata is schema-aware, qualified input should be rejected rather than silently
redirected.

Relevant code: `src/compiler/dp_ownership_graph.cpp:52-55`.

### 8. Low: metadata retrieval has an unsafe lifetime under concurrent mutation

`GetTableMetadata` returns a pointer after releasing the metadata-manager mutex. The graph builder
then copies through that pointer. A concurrent metadata insertion or clear can invalidate it.

Add a manager method that returns a copy while still holding the lock, or obtain one immutable
metadata snapshot for the whole analysis.

Relevant code: `src/compiler/dp_ownership_graph.cpp:175-180` and
`src/metadata/privacy_metadata_manager.cpp:66-88`.

## Missing draft components

These are scope gaps rather than defects if this PR is explicitly presented as the first diagnostic
prototype:

- bottom-up analysis of selection, projection, key joins, and general equijoins;
- public/enforced MMF declarations and propagation through intermediate relations;
- the private influence histogram and `1 + 2U_schema` calibration;
- a provably stable `ClipInfluence` transformation;
- private and stable group-contribution bounding;
- private per-PU/per-group SUM bounding, including the required
  `G_priv(1 + 2U_priv)` histogram sensitivity;
- public-domain grouping or private partition selection; and
- adaptive composition of all bound-selection and final-release stages.

The clipping operators should not be implemented until their neighboring-database stability is
specified. Enforcing a degree or contribution cap on the output relation alone does not prove that
the transformation has bounded sensitivity.

## What is implemented well

- The base-table recurrence maps cleanly to `c(R)`, `o(R)`, and `c(R)(o(R)-1)`.
- Multiple ownership paths are conservatively summed for the schema-style bound and deduplicated for
  the exact observed degree.
- Composite privacy keys and links are handled without hashing collisions.
- Overflow checks fail closed.
- Cycles, missing keys, orphaned links, non-unique referenced keys, and multiple distinguished PUs
  are rejected explicitly.
- The SQLLogicTest passes all 135 assertions, and the Ninja build is clean.

## Test gaps

The current tests cover parallel links, a chain, composite keys, and several invalid inputs. Before
expanding the feature, add deterministic tests for:

- a real reconverging diamond DAG;
- a general query join that creates multi-PU provenance from singly owned inputs;
- an ownership-consistent key join where `o(join) = o(FK side)`;
- temporary tables and uncommitted changes, if the diagnostic is intended to support them;
- qualified table names; and
- larger layered DAGs to prevent accidental path-explosion regressions.

## Recommended next step

Split the current module into two concepts:

1. `DPOwnershipSchemaAnalyzer`: pure public metadata and logical-plan inference producing the formal
   `U_schema`.
2. `privacy_ownership_bounds_debug`: explicitly unsafe observed-data diagnostics used only to compare
   the schema bound with the current instance.

That separation prevents exact scanned maxima from accidentally flowing into DP calibration and
gives the compiler a small, testable abstraction matching Dandan's draft.
