# Graviton revision rerun, November 2026

The DuckDB 2.0 branch is rebased onto main `52d4a3b`. The Graviton host is
`ec2-3-136-25-192.us-east-2.compute.amazonaws.com`: 16 Neoverse-V2 cores,
32 GB RAM, Ubuntu 26.04, Clang 22.1.2, and a local NVMe SSD mounted at
`/mnt/asa`. The SSD was blank after instance restart; datasets are regenerated.

`/mnt/asa/privacy` holds main; `/mnt/asa/privacy2` holds the upgrade branch.
Both use separate build directories and database copies. The upgrade's pinned
DuckDB source is `d54cbbe3df17e8e5ce68312f483b4155836112aa`. TPC-H and TPC-DS
generator extensions are built from that same source and installed locally.

Preparation/build services record their commits, settings, logs, and exit codes
under `/mnt/asa/repro`. Run the suite after starting these services:

```sh
sudo systemd-run --unit=asa-full-revision-20261110 --uid=ubuntu \
  --working-directory=/mnt/asa/privacy2 \
  /bin/bash /mnt/asa/privacy2/benchmark/run_graviton_full_revision.sh \
  /mnt/asa /mnt/asa/results/duckdb2_revision_20261110
```

Order: matched main TPC-H control, 2.0 TPC-H and its sample-count sweep,
ClickBench, SSB, MEPS, TPC-DS validation/performance, DP utility/stability,
then aggregation microbenchmarks. Analytical timings retain five measured
executions after warmup; DP configurations retain their existing three runs.
SSB and MEPS run approximate and exact SUM. Microbenchmarks retain their
existing four-thread setup, one warmup, three runs, and slow-query cutoff.
All stages run sequentially. A failed stage is logged and subsequent stages
continue; individual TPC-DS query failures are recorded in its CSVs.

The microbenchmark driver uses a separate build tree and never deletes the
analytical build. Its summary now computes a true median: the old awk expression
selected the minimum sorted observation. Raw timing samples remain in the CSVs.
SUM accuracy compares exact and approximate samples on the same unscaled units
for both errors and variances. These measurement corrections must be considered
when comparing old summaries.

Copy CSVs, configs, manifests, plans, and logs locally before stopping the instance.
The local collection is `benchmark/results/graviton_duckdb2_20261110/`; prior
paper CSVs and the October 28 five-run Graviton controls are retained under
`previous/`. No paper text or figures are updated by this driver.
