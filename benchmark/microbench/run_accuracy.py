"""Repeat the existing ten-distribution SUM accuracy experiment on a common scale."""
import argparse
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = (Path(__file__).parent / "approx_sum_stability.sql").read_text()
marker = source.index("-- test query:")
setup = source[:marker]
start = source.index("WITH bit_positions", marker)
query = source[start:source.index(";", start)]
# as_sum returns 2x the sampled totals. Rescale once, before computing both
# squared errors and the approximate variance, so every statistic uses the
# same units as independently computed sample sums.
query = query.replace("as_sum(priv_hash(hash(rowid)), val) as approx_counters",
                      "list_transform(as_sum(priv_hash(hash(rowid)), val), lambda x: x / 2.0) as approx_counters")
query = query.replace("a.approx_counters[x+1] / 2", "a.approx_counters[x+1]")
query = query.replace("x -> abs(x)", "lambda x: abs(x)")
path = str(args.output.resolve()).replace("'", "''")
sql = ("SET threads=4; SET memory_limit='30GB'; SET priv_rewrite=false; "
       "SET pac_mi=1; SET privacy_seed=42; SELECT setseed(0.42);\n" + setup
       + "\nCOPY (" + query + ") TO '" + path + "' (HEADER, DELIMITER ',');")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.with_suffix(".sql").write_text(sql)
subprocess.run([str(args.binary.resolve()), "-batch"], input=sql, text=True, check=True)
