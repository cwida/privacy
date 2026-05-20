# IMDb/JOB Benchmark

`imdb_benchmark` runs PAC and `dp_elastic` on an IMDb/JOB-style workload with person-level privacy.

The benchmark uses the JOB schema and expects CSV data from the JOB IMDb tarball. By default it downloads:

```bash
https://bonsai.cedardb.com/job/imdb.tgz
```

The tarball is large, so use `--micro` for quick local checks.

```bash
cmake --build build/release --target imdb_benchmark
./build/release/extension/privacy/imdb_benchmark --micro --out /tmp/imdb_micro.csv
./build/release/extension/privacy/imdb_benchmark --db imdb.db --out benchmark/imdb_results.csv
```

The privacy unit is `name.id`, not `title.id`, because titles are public objects while people are the sensitive entities. The default query file is `benchmark/imdb/queries.sql`; broader title joins are in `queries_pac_join.sql` and may be rejected by `dp_elastic` because they are not a single linear privacy-link chain to `name`.
