#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DBGEN_DIR = ROOT / "benchmark" / "jcch" / "dbgen.JCC-H"
TABLES = ["nation", "region", "part", "supplier", "partsupp", "customer", "orders", "lineitem"]


def run(cmd, cwd, timeout=None):
	start = time.monotonic()
	display = [str(c) if len(str(c)) <= 160 else str(c)[:157] + "..." for c in cmd]
	print(f"[jcch] {' '.join(display)}", flush=True)
	proc = subprocess.run(cmd, cwd=str(cwd), text=True, timeout=timeout)
	if proc.returncode != 0:
		raise RuntimeError(f"command failed with exit code {proc.returncode}: {' '.join(str(c) for c in cmd)}")
	print(f"[jcch] done in {time.monotonic() - start:.1f}s", flush=True)


def build_dbgen():
	if (DBGEN_DIR / "dbgen").exists() and (DBGEN_DIR / "qgen").exists():
		return
	run(["make", "CC=gcc -std=gnu89"], DBGEN_DIR)


def variant_dir(base_dir, sf, variant):
	sf_text = str(sf).replace(".", "_")
	return base_dir / f"sf{sf_text}_{variant}"


def db_path(base_dir, sf, variant):
	sf_text = str(sf).replace(".", "_")
	return base_dir / f"jcch_actual_sf{sf_text}_{variant}.db"


def generate_tbl(sf, variant, out_dir, force):
	out_dir.mkdir(parents=True, exist_ok=True)
	if force:
		for path in out_dir.glob("*.tbl"):
			path.unlink()
	if all((out_dir / f"{table}.tbl").exists() for table in TABLES):
		return
	cmd = [
	    str(DBGEN_DIR / "dbgen"),
	    "-b",
	    str(DBGEN_DIR / "dists.dss"),
	    "-f",
	    "-s",
	    str(sf),
	    "-q",
	]
	if variant == "skew":
		cmd.append("-k")
	run(cmd, out_dir, timeout=3600)


def load_duckdb(duckdb, tbl_dir, out_db, force):
	if force and out_db.exists():
		out_db.unlink()
	if out_db.exists():
		return
	ddl = (DBGEN_DIR / "dss.ddl").read_text().replace(" NOT NULL", "").lower()
	statements = ["LOAD privacy;", ddl]
	for table in TABLES:
		statements.append(f"COPY {table} FROM '{tbl_dir / (table + '.tbl')}' (DELIMITER '|');")
	statements.extend(
	    [
	        "PRAGMA clear_privacy_metadata;",
	        "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);",
	        "ALTER TABLE customer SET PU;",
	        "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);",
	        "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);",
	        "CHECKPOINT;",
	    ]
	)
	run([duckdb, str(out_db), "-c", "\n".join(statements)], ROOT, timeout=3600)


def main():
	parser = argparse.ArgumentParser(description="Generate and load actual JCC-H data from ldbc/dbgen.JCC-H.")
	parser.add_argument("--sf", type=float, default=1.0)
	parser.add_argument("--variant", choices=["normal", "skew", "both"], default="both")
	parser.add_argument("--out-dir", default="/tmp/jcch_actual")
	parser.add_argument("--duckdb", default=str(ROOT / "build/release/duckdb"))
	parser.add_argument("--force", action="store_true")
	parser.add_argument("--remove-tbl", action="store_true", help="delete generated .tbl files after DuckDB load")
	args = parser.parse_args()

	base_dir = Path(args.out_dir)
	variants = ["normal", "skew"] if args.variant == "both" else [args.variant]
	build_dbgen()
	for variant in variants:
		tbl_dir = variant_dir(base_dir, args.sf, variant)
		out_db = db_path(base_dir, args.sf, variant)
		generate_tbl(args.sf, variant, tbl_dir, args.force)
		load_duckdb(args.duckdb, tbl_dir, out_db, args.force)
		print(f"[jcch] wrote {out_db}", flush=True)
		if args.remove_tbl:
			shutil.rmtree(tbl_dir)


if __name__ == "__main__":
	try:
		main()
	except Exception as exc:
		print(f"error: {exc}", file=sys.stderr)
		sys.exit(1)
