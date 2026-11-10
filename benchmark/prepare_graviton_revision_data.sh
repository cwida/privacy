#!/usr/bin/env bash
set -euo pipefail
root="${1:?Usage: prepare_graviton_revision_data.sh ROOT}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$root/data" "$root/repro/data_prepare"
exec > >(tee -a "$root/repro/data_prepare/run.log") 2>&1
printf 'running\n' > "$root/repro/data_prepare/exit_code"
trap 'printf "%s\n" "$?" > "$root/repro/data_prepare/exit_code"' EXIT
cd "$repo"
if [ ! -d "$root/ssb-dbgen" ]; then
  git clone https://github.com/electrum/ssb-dbgen.git "$root/ssb-dbgen"
  git -C "$root/ssb-dbgen" checkout 219403ad7d1dd32ae1f97b5553abf92129fccd7f
  git -C "$root/ssb-dbgen" apply "$repo/benchmark/ssb/dbgen-macos.patch"
fi
make -C "$root/ssb-dbgen" dbgen CC=clang-22 \
  CFLAGS='-O2 -std=gnu89 -include unistd.h -D_GNU_SOURCE -DDBNAME=\"dss\" -DLINUX -DDB2 -DSSBM'
if [ ! -f "$root/data/ssb_sf30/generation_complete" ]; then
  bash benchmark/ssb/prepare.sh "$root/ssb-dbgen" "$root/data/ssb_sf30" 30
fi
while pgrep -x apt-get >/dev/null; do sleep 5; done
python3 -m venv --system-site-packages "$root/prepare-env"
"$root/prepare-env/bin/python" -m pip freeze > "$root/repro/data_prepare/python_requirements.txt"
for workload in damico2006 ahrq2016; do
  if [ ! -f "$root/data/meps/$workload/manifest.json" ]; then
    "$root/prepare-env/bin/python" benchmark/meps_brr/prepare.py "$root/data/meps/$workload" --workload "$workload"
  fi
done
if [ ! -f "$repo/hits.parquet" ]; then
  curl --fail --location --retry 3 https://datasets.clickhouse.com/hits_compatible/hits.parquet \
    --output "$repo/hits.parquet.part"
  mv "$repo/hits.parquet.part" "$repo/hits.parquet"
fi
sha256sum "$repo/hits.parquet" > "$root/repro/data_prepare/clickbench.sha256"
