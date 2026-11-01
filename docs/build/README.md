# Building the PAC DuckDB Extension

## Prerequisites

- **git** (with submodule support)
- **cmake** (>= 3.14)
- **ninja** (optional but recommended — significantly faster builds)
- **C++ compiler** with C++17 support (clang recommended, especially for benchmarks)

## Clone

```bash
git clone --recurse-submodules https://github.com/cwida/privacy.git
cd privacy
```

If you already cloned without `--recurse-submodules`, run:

```bash
git submodule update --init --recursive
```

## Build

```bash
GEN=ninja make        # release build (recommended)
make                  # release build without ninja
make debug            # debug build
```

The extension is built into `build/release/extension/privacy/privacy.duckdb_extension`.

The Makefile sets `OVERRIDE_GIT_DESCRIBE=v2.0.0` so the loadable extension
uses the released DuckDB ABI label. When configuring CMake directly, pass
`-DOVERRIDE_GIT_DESCRIBE=v2.0.0` as well. DuckDB 2.0 requires explicit static
extension registration; `extension_config.cmake` enables it for the CLI,
tests, and benchmark runners.

The compiler uses DuckDB 2.0's default CTE-based decorrelation. No legacy
`delim_join_as_cte` setting is required. CTE readers carry the corresponding
mask bindings and counter types, and intermediate expressions remain per-world
until the output projection finalizes them.

DuckDB 2.0 can infer wider DECIMAL types for arithmetic than DuckDB 1.5.
The inferred width selects the SUM state (including the INT128 path), so
approximate outputs can differ across engine versions even with the same seed.

## Run Tests

```bash
# all tests
make test

# PAC SQL tests only
./build/release/test/unittest --test-dir . "test/sql/pac*"

# C++ unit tests
./build/release/extension/privacy/pac_test_runner
```

## Build Flags

Compile-time flags control algorithm variants, primarily for benchmarking and scientific reproducibility.
Set them via `cmake` defines (e.g., `-DPAC_NOBUFFERING=1`).

| Flag | Effect |
|------|--------|
| `PAC_NOBUFFERING` | Disable input buffering (lazy allocation) |
| `PAC_NOCASCADING` | Pre-allocate all accumulator levels |
| `PAC_NOSIMD` | No-cascading with SIMD-unfriendly update kernels and auto-vectorization disabled |
| `PAC_NOBOUNDOPT` | Disable bound optimization (only affects min/max) |
| `PAC_SIGNEDSUM` | Disable handling negative values in signed sums using separate (negated) counters |
| `PAC_EXACTSUM` | Disable approximate sum optimization, use exact cascading (implies signedsum) |

## Updating the DuckDB Version

The DuckDB version is pinned via the `duckdb` and `extension-ci-tools` submodules.
To update to a new release:

```bash
cd duckdb && git fetch --tags && git checkout v2.0.0
cd ../extension-ci-tools && git fetch origin && git checkout ebdcc1961780b0ce1272a0d7637dc58447c34885
cd ..
git add duckdb extension-ci-tools
git commit -m "Bump DuckDB to v2.0.0"
```

The upgrade pins DuckDB 2.0.0 and the CI-tools commit shown above.
When changing versions, update both submodule pins and the references in
`.github/workflows/MainDistributionPipeline.yml`.
