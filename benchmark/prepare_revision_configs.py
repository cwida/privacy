"""Prepare fresh Graviton run configurations without changing paper configurations."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("root", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
args.output.mkdir(parents=True, exist_ok=True)
tpch_db = str(args.root / "data/tpch_sf30_duckdb2.db")
for source in sorted((repo / "benchmark/configs/utility").glob("*.json")):
    if source.name.startswith("as_tpcds"):
        continue
    config = json.loads(source.read_text())
    config["out"] = str(args.output.parent / "csv" / (source.stem + ".csv"))
    if source.name.startswith("as_tpch"):
        config.update(db=tpch_db, queries_dir=str(repo / "benchmark"))
    elif source.name.startswith("as_clickbench"):
        config.update(db=str(args.root / "data/clickbench_duckdb2.db"),
                      queries_dir=str(repo / "benchmark/clickbench/clickbench_queries"))
    elif source.name.startswith("dp_tpch"):
        for dataset in config.get("datasets", []):
            dataset["db"] = tpch_db
    else:
        continue
    (args.output / source.name).write_text(json.dumps(config, indent=2) + "\n")

config = json.loads((repo / "benchmark/configs/utility/as_tpcds_sf30.json").read_text())
config.update(db=str(args.root / "data/tpcds_sf30_duckdb2/data.db"),
              queries_dir=str(args.root / "data/tpcds_sf30_duckdb2/queries"),
              out_dir=str(args.output.parent / "tpcds_sf30"),
              schema=str(repo / "benchmark/tpcds/schema.sql"),
              as_queries_dir=str(repo / "benchmark/tpcds/as_queries"),
              reference_queries_dir=str(repo / "benchmark/tpcds/reference_queries"),
              extension_directory=str(args.root / "data/tpcds_extensions_duckdb2"))
(args.output / "as_tpcds_sf30.json").write_text(json.dumps(config, indent=2) + "\n")
config.update(sf=1, threads=1, repetitions=1,
              db=str(args.root / "data/tpcds_sf1_duckdb2/data.db"),
              queries_dir=str(args.root / "data/tpcds_sf1_duckdb2/queries"),
              out_dir=str(args.output.parent / "tpcds_sf1_validation"))
(args.output / "as_tpcds_validate_sf1.json").write_text(json.dumps(config, indent=2) + "\n")

main = json.loads((repo / "benchmark/configs/utility/as_tpch_sf30.json").read_text())
main.update(db=str(args.root / "data/tpch_sf30_main.db"),
            queries_dir=str(args.root / "privacy/benchmark"),
            out=str(args.output.parent / "csv/as_tpch_sf30_main.csv"))
(args.output / "as_tpch_sf30_main.json").write_text(json.dumps(main, indent=2) + "\n")
