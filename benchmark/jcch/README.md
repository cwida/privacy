# Actual JCC-H Data

This directory vendors the LDBC JCC-H generator as a submodule:

```bash
git submodule update --init benchmark/jcch/dbgen.JCC-H
```

JCC-H supports two data/query variants:

- `normal`: generated without `dbgen -k`
- `skew`: generated with `dbgen -k`, enabling join-crossing correlations

Generate and load both variants into DuckDB:

```bash
python3 benchmark/jcch/generate_actual_jcch.py --sf 1 --variant both
```

The loader lowercases the TPC-H schema names and removes `NOT NULL` constraints
before loading, because the skewed generator can emit empty fields. It also
adds customer-level privacy metadata:

```sql
ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);
ALTER TABLE customer SET PU;
ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);
ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);
```
