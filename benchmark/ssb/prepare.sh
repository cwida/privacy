#!/usr/bin/env bash
# WIP: run the pinned SSB generator outside the measured query phase.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo 'Usage: bash benchmark/ssb/prepare.sh DBGEN_REPO NEW_DATA_DIR [SCALE_FACTOR]' >&2
  exit 2
fi
generator="$(cd "$1" && pwd)"
data_dir="$2"
scale="${3:-1}"
if [ -e "$data_dir" ]; then
  echo "Refusing existing data directory: $data_dir" >&2
  exit 1
fi
mkdir -p "$data_dir"
data_dir="$(cd "$data_dir" && pwd)"
cd "$data_dir"
TZ=UTC DSS_CONFIG="$generator" "$generator/dbgen" -s "$scale" -T a
git -C "$generator" rev-parse HEAD > generator_commit.txt
git -C "$generator" diff > generator_changes.patch
printf '%s\n' "$scale" > scale_factor.txt
printf 'table,rows\n' > row_counts.csv
for table in customer supplier part date lineorder; do
  printf '%s,%s\n' "$table" "$(wc -l < "$table.tbl" | tr -d ' ')" >> row_counts.csv
done
shasum -a 256 customer.tbl supplier.tbl part.tbl date.tbl lineorder.tbl > SHA256SUMS
# Written last: a failed generator cannot be mistaken for a complete dataset.
touch generation_complete
