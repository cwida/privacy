# SQLStorm Sanity Checks

SQLStorm is an optional coverage workload for the generic DP benchmark runner. It is not part of the paper result
matrix. Queries come from the `benchmark/sqlstorm/SQLStorm/` submodule.

## TPC-H Smoke Test

```bash
cmake --build build/release --target dp_benchmark_runner

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/sanity/dp_sqlstorm_tpch_smoke.json --dry-run

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/sanity/dp_sqlstorm_tpch_smoke.json
```

This config runs SQLStorm TPC-H query `10001.sql` with elastic DP and SAA on an SF0.001 database.

## TPC-H and StackOverflow Smoke Test

```bash
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/sanity/dp_sqlstorm_smoke.json --dry-run

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/sanity/dp_sqlstorm_smoke.json
```

The combined config selects one query from each workload. The StackOverflow setup uses
`benchmark/sqlstorm/pac_stackoverflow_schema.sql` to identify `Users.Id` as the privacy unit and connect dependent
tables. Dataset downloads and generated databases are local benchmark artifacts and are ignored by Git.

These configs use broad smoke-test bounds. They validate query setup and execution, not utility claims.
