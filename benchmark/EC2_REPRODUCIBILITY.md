# SSB and MEPS revision experiments on Graviton4

The October 28, 2026 run uses an AWS `c8gd.4xlarge` in `us-east-2`:
16 physical Graviton4 cores (Neoverse-V2), 32 GB RAM, and a 950 GB local
NVMe instance SSD (884.8 GiB). The SSD is mounted at `/mnt/asa` with ext4.
The 120 GB root EBS volume holds the operating system. Data, builds, spill
files, databases, and measurements reside on the local SSD.

The operating system is Ubuntu 26.04.1 LTS. DuckDB is v1.5.4, submodule
commit `08e34c447bae34eaee3723cac61f2878b6bdf787`. The extension starts from
`0bc3082d6476df65c26596ca8c3fbc25720736f4`, with the current MEPS working-tree
changes included in a source snapshot. The server preserves the snapshot,
its SHA-256, the tracked diff, and source status under `/mnt/asa/repro`.
The initial source archive includes untracked MEPS SQL. The supplemental converter
copy fixes the official BRR download URL to include its `h036brr/` directory;
the supplemental hashes identify this correction and the driver/documentation.
Use that converter copy over the archived version when reproducing the run.
It excludes local benchmark results and conversation transcripts. The five-run
rerun also preserves the updated runners and a tracked diff in its own `repro/`
directory; these replace the initial archive's benchmark files.

The compiler is Ubuntu Clang 22.1.2, a patch-version difference from the
paper's stated Clang 22.1.0. CMake configures Release (`-O3 -DNDEBUG`),
using the default architecture flags. The saved CMake caches and compilation
database record the effective configuration. Builds use four jobs; query
execution uses 16 threads. SSB sets DuckDB's memory limit to 30 GB.
MEPS retains its runner's default memory setting. No other benchmarks run
concurrently on the machine or use its build directory.

## Preparation and launch

Install dependencies on a fresh Ubuntu ARM64 instance:

```sh
sudo apt-get update
sudo apt-get install -y clang-22 cmake ninja-build build-essential git \
  python3-venv libssl-dev pkg-config
```

Mount the local SSD and give the benchmark user ownership. Formatting is only
appropriate for a verified blank device; the driver does not format disks.
Copy the source snapshot to the SSD and extract it as `/mnt/asa/privacy`.
For a Git checkout instead, initialize `duckdb` and `extension-ci-tools`,
and ensure all MEPS SQL files and the runner match the archived snapshot.

```sh
sudo systemd-run --unit=asa-revision-five --uid=ubuntu \
  --working-directory=/mnt/asa/privacy \
  /bin/bash /mnt/asa/privacy/benchmark/run_graviton_revision.sh /mnt/asa 16 \
    /mnt/asa/results/revision_5runs_ordered_20261028
```

The service survives SSH disconnection and laptop sleep. It does not survive
instance termination; instance-store data is also lost when the EC2 instance
is stopped. Copy results and provenance to durable storage before stopping it.

```sh
systemctl status asa-revision-five --no-pager
tail -40 /mnt/asa/results/revision_5runs_ordered_20261028/repro/run.log
cat /mnt/asa/results/revision_5runs_ordered_20261028/repro/exit_code
```

An exit code of zero is written only after the complete suite succeeds.
The driver refuses existing result directories via the benchmark wrappers;
use a new result location for another complete run. After an interruption,
inspect the log and reuse completed input data and builds explicitly rather
than overwriting measurements.

## Workloads and repetitions

SSB generates SF30 using `electrum/ssb-dbgen` commit
`219403ad7d1dd32ae1f97b5553abf92129fccd7f` and the checked-in buffer/RNG
safety patch. Although named `dbgen-macos.patch`, these fixes also apply on
Linux; the driver builds with `-DLINUX -D_GNU_SOURCE` (the latter exposes
the legacy generator's `O_LARGEFILE` on current glibc). Generation records the patch, row
counts, and SHA-256 hashes. All 13 queries use the original five-table schema
and query-time customer membership, with 64 overlapping worlds.

```sh
SSB_MEMORY_LIMIT=30GB bash benchmark/ssb/run.sh \
  /mnt/asa/data/ssb_sf30 /mnt/asa/results/revision_5runs_ordered_20261028/ssb_sf30 16 5
```

The wrapper runs approximate and exact SUM, each after its excluded warmup.
Each timed series has five repetitions and reports a median. The original
`ORDER BY` clauses remain in the timed SQL. Naive time is world 0 latency ×64;
the remaining worlds are executed for correctness only. AS ordering on list
values is batch ordering, not 64 independently sorted result sets. Exact measures AS only, reusing
approximate-run ordinary SQL timings for plots; it still computes its own
64-world correctness reference. See [SSB methodology](ssb/README.md).

MEPS prepares both Damico 2006 (64 official BRR replicates) and AHRQ 2016
(128 replicates) directly from public inputs. Its converter records URLs,
input hashes, and row counts. Python/pandas only prepares data; all timed
queries execute in the C++ DuckDB runner. The exact installed Python package
versions are exported to `repro/python_requirements.txt`; use that file with
`pip install -r` when recreating the conversion environment.

```sh
bash benchmark/meps_brr/run.sh /mnt/asa/data/meps \
  /mnt/asa/results/revision_5runs_ordered_20261028/meps 16 5
```

Each database has one warmup (run 0) followed by five measured executions
(runs 1--5). Naive time is replicate 1 latency ×64 or ×128; all official
replicates still supply correctness checks and BRR standard errors.
Both workloads compare full-sample SQL, repeated replicate SQL, conditional
SQL, and SIMD-AS. See [MEPS methodology and validation](meps_brr/README.md).

## Results and plots

Retain the complete `results/` and `repro/` trees, input manifests, and SSB
generation metadata. The raw CSVs include correctness/error measurements;
each wrapper also saves SQL, EXPLAIN plans, build logs, and CMake caches.
Completion requires checking all query/mode/repetition counts and numerical
validation, in addition to the service exit code.

After copying results locally, use the existing paper-style plotters:

```sh
Rscript benchmark/ssb/plot_ssb_results.R /path/to/results/ssb_sf30 30 16
Rscript benchmark/meps_brr/plot_meps_results.R /path/to/results/meps 16
```

Both plots use medians of five warm measured executions. Full-data
DuckDB is a cost reference, not an answer-equivalent sampled-world baseline.

## Earlier three-repetition run (superseded methodology)

The earlier service completed with exit code 0. It omitted SSB sorting and
timed every naive replicate; its MEPS plot used means. These results remain
archived separately and must not be mixed with the five-run rerun.
Local artifacts are under
`benchmark/results/graviton_revision_20261028/`: `results/` contains raw
measurements and figures, `repro/` contains source/environment records, and
`data_manifests/` contains input provenance. `validation_summary.json` records
the repetition, coverage, and numerical audit.

SSB SF30 contains 179,998,484 LINEORDER rows. Both builds accepted and executed
all 13 original queries; the explicit AS rewrites checked 131,730 world/group
values per build. Exact had zero mismatches after FLOAT output rounding.
Approximate SUM's largest relative error was 0.327%, with a value-weighted mean
of 0.0500%. Geometric means of per-query median runtime ratios were:

| SSB SUM mode | Speedup over repeated SQL | Runtime / full-data DuckDB |
| --- | ---: | ---: |
| Approximate | 90.6× | 1.191× |
| Exact | 90.8× | 1.188× |

MEPS completed all seven queries in both workloads, four SQL modes, and both
builds across three fresh-database suites. Damico's published one-decimal
point estimates and both BRR SE conventions passed. The sum of per-query
mean measured times was 402.4 ms for approximate AS and 327.2 ms for exact AS,
versus 2,033.7 ms for conditional SQL and 4,674.9 ms for repeated SQL (using
the approximate build's ordinary-SQL controls consistently). These correspond
to speedups of 5.05×/6.22× over conditional SQL and 11.6×/14.3× over repeated
SQL. The largest normalized numerical errors, including BRR SE comparisons,
were 0.0141% for approximate AS and 0.0000183% for exact AS.

The SSB figure uses medians; the MEPS figure uses means and sample SDs, as
described above. These are different summaries and should retain those labels.
