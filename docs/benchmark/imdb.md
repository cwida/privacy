# IMDb/JOB Benchmark

`imdb_benchmark` exercises PAC and elastic DP on an IMDb/JOB-style workload with person-level privacy. It is an
additional development benchmark, not part of the paper result matrix.

## Quick Check

```bash
cmake --build build/release --target imdb_benchmark
build/release/extension/privacy/imdb_benchmark --micro --out /tmp/imdb_micro.csv
```

## Full Workload

```bash
build/release/extension/privacy/imdb_benchmark \
  --db imdb.db \
  --queries benchmark/imdb/queries.sql \
  --out benchmark/results/imdb.csv
```

The runner downloads the JOB IMDb data from the CedarDB mirror when needed. Use `--data-dir` or `--url` to override
the input location.

The privacy unit is `name.id`: people are sensitive entities, while titles are public objects. Queries in
`benchmark/imdb/queries_pac_join.sql` use broader title joins and may be rejected by elastic DP when the joins do not
form one linear privacy-link chain to `name`.
