#!/usr/bin/env python3
"""
MIA Simulation: PAC vs dp_elastic vs non-private

Game: adversary issues K repeated 2-hop COUNT queries (different random seeds)
      and decides whether a specific target customer is in the database.

Adversary score per query: |y - r_out| - |y - r_in|  (positive → predict IN)
Accumulated over K queries, then bootstrap AUC measured.

Usage (from repo root):
    python3 benchmark/tpch/mia_simulation.py [--trials N] [--bootstrap B]
"""

import argparse
import os
import random
import subprocess
import sys

DUCKDB  = "build/release/duckdb"
DB      = "tpch_sf1.db"
PAC_EXT = "build/release/extension/privacy/privacy.duckdb_extension"

# 2-hop COUNT — strongest MIA signal (target contributes all their lineitems)
BASE_Q = (
    "SELECT COUNT(*) FROM customer"
    " JOIN orders ON c_custkey = o_custkey"
    " JOIN lineitem ON o_orderkey = l_orderkey"
)

MODES = [
    ("Non-private",  dict(mode="nonprivate")),
    ("PAC mi=1/128", dict(mode="pac", pac_mi=1.0 / 128)),
    ("PAC mi=1/8",   dict(mode="pac", pac_mi=1.0 / 8)),
    ("DP ε=0.067",   dict(mode="dp",  epsilon=0.067)),
    ("DP ε=1.0",     dict(mode="dp",  epsilon=1.0)),
]

K_VALS = [1, 2, 5, 10, 25, 50, 100]


def run_duckdb(sql):
    proc = subprocess.run(
        [DUCKDB, "-csv", DB, "-c", sql],
        capture_output=True, text=True,
    )
    vals = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            vals.append(float(line))
        except ValueError:
            pass
    return vals


PAC_SCHEMA = [
    "PRAGMA clear_privacy_metadata;",
    "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);",
    "ALTER TABLE customer SET PU;",
    "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);",
    "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);",
]


def build_preamble(cfg):
    m = cfg["mode"]
    lines = [f"LOAD '{PAC_EXT}';", "SET threads=1;"]
    if m == "nonprivate":
        lines += ["SET privacy_mode='pac';", "SET privacy_noise=false;"]
    elif m == "pac":
        lines += ["SET privacy_mode='pac';", f"SET pac_mi={cfg['pac_mi']!r};"]
    elif m == "dp":
        lines += ["SET privacy_mode='dp_elastic';", f"SET dp_epsilon={cfg['epsilon']!r};", "SET dp_delta=1e-6;"]
    lines += PAC_SCHEMA
    return lines


def run_trials(cfg, query_sql, n_trials, batch_size=10):
    """Run n_trials independent noisy queries. Returns list of floats."""
    pre = build_preamble(cfg)
    results = []
    for start in range(0, n_trials, batch_size):
        n_batch = min(batch_size, n_trials - start)
        seeds = [1337 + (start + i) * 997 for i in range(n_batch)]
        lines = pre[:]
        for s in seeds:
            lines.append(f"SET privacy_seed={s};")
            lines.append(query_sql + ";")
        vals = run_duckdb("\n".join(lines))
        results.extend(vals[:n_batch])
    return results[:n_trials]


def bootstrap_auc(scores_in, scores_out, k, n_bootstrap=2000, rng_seed=42):
    """
    Bootstrap AUC for K accumulated queries.
    Adversary draws K scores from each world and picks the higher total.
    """
    rng = random.Random(rng_seed)
    wins = 0.0
    for _ in range(n_bootstrap):
        si = sum(rng.choices(scores_in,  k=k))
        so = sum(rng.choices(scores_out, k=k))
        wins += 1.0 if si > so else (0.5 if si == so else 0.0)
    return wins / n_bootstrap


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trials",    type=int, default=200, help="trials per mode per world")
    ap.add_argument("--bootstrap", type=int, default=2000)
    args = ap.parse_args()

    for path in [DB, DUCKDB, PAC_EXT]:
        if not os.path.exists(path):
            sys.exit(f"Not found: {path}")

    # Find target: customer with most orders (strongest MIA signal).
    # PRAGMA clear_privacy_metadata avoids the PAC protected-column restriction on o_custkey.
    print("Finding target customer...", end=" ", flush=True)
    t_vals = run_duckdb(
        "PRAGMA clear_privacy_metadata;\n"
        "SELECT o_custkey FROM orders GROUP BY o_custkey ORDER BY COUNT(*) DESC LIMIT 1;\n"
    )
    target = int(t_vals[0])

    # True values (no noise)
    preamble_true = (
        f"LOAD '{PAC_EXT}';\nSET threads=1;\nSET privacy_mode='pac';\nSET privacy_noise=false;\n"
        + "\n".join(PAC_SCHEMA) + "\n"
    )
    q_out = BASE_Q + f" WHERE c_custkey != {target}"
    r_in  = run_duckdb(preamble_true + BASE_Q + ";\n")[0]
    r_out = run_duckdb(preamble_true + q_out  + ";\n")[0]
    delta = r_in - r_out
    print(f"customer {target}  (r_in={r_in:.0f}, r_out={r_out:.0f}, Δ={delta:.0f} lineitems)")
    print()

    # Collect per-trial LLR scores for each mode
    # score = |y - r_out| - |y - r_in|  →  positive means closer to IN
    all_scores = {}
    for label, cfg in MODES:
        print(f"  [{label:<14}] IN trials...", end=" ", flush=True)
        y_in  = run_trials(cfg, BASE_Q, args.trials)
        print("OUT trials...", end=" ", flush=True)
        y_out = run_trials(cfg, q_out,  args.trials)
        sc_in  = [abs(y - r_out) - abs(y - r_in) for y in y_in]
        sc_out = [abs(y - r_out) - abs(y - r_in) for y in y_out]
        all_scores[label] = (sc_in, sc_out)
        # Quick sanity: mean score should be positive for IN, negative for OUT
        mean_in  = sum(sc_in)  / len(sc_in)
        mean_out = sum(sc_out) / len(sc_out)
        print(f"done  (mean score IN={mean_in:+.1f}, OUT={mean_out:+.1f})")

    print()

    # AUC table
    col_w = 13
    header = f"{'K':>5}" + "".join(f"  {l:>{col_w}}" for l, _ in MODES)
    sep    = "─" * len(header)

    print("MIA AUC: adversary repeats 2-hop COUNT K times, accumulates evidence")
    print(f"Target: customer {target}  |  Δ = {delta:.0f} lineitems removed in D_out")
    print(f"AUC = 0.50 → random guess  |  AUC = 1.00 → perfect attack")
    print()
    print(header)
    print(sep)

    for k in K_VALS:
        if k > args.trials:
            break
        row = f"{k:>5}"
        for label, _ in MODES:
            sc_in, sc_out = all_scores[label]
            auc = bootstrap_auc(sc_in, sc_out, k, n_bootstrap=args.bootstrap)
            row += f"  {auc:>{col_w}.4f}"
        print(row)

    print()
    print("Notes:")
    print("  K   = number of independent repeated queries (each with a fresh random seed)")
    print("  AUC computed by bootstrap: adversary accumulates LLR scores over K queries")
    print("  PAC mi=1/128 ≈ DP ε=0.067 (equivalent privacy level, Sridhar thesis)")
    print("  Non-private AUC=1.0 because query result reveals Δ exactly (no noise)")


if __name__ == "__main__":
    main()
