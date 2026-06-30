#!/usr/bin/env python3
"""
Composition MIA: PAC vs DP — budget depletion with single-cell and GROUP BY queries.

Key question: for equivalent per-cell noise, how does formal budget depletion
differ between PAC and DP as the adversary issues more queries?

PAC formal budget:  K × G × mi  (charges per output cell)
DP  formal budget:  K × ε       (charges per query, regardless of G)

With mi=1/128 and ε=0.067 (≈ equivalent per-cell privacy):
  G=1 query:  PAC = 1/128 = 0.0078 nats/query   DP = 0.067 nats/query
  G=7 query:  PAC = 7/128 = 0.055 nats/query    DP = 0.067 nats/query

→ For G=1: DP is 8.6× more expensive per query than PAC (same noise, different accounting)
→ For G=7: PAC and DP are roughly equivalent per query in budget cost

If the budget accounting is correct, AUC at the same *formal budget spent* should
be the same regardless of mechanism (PAC or DP) — both leak the same information.

Usage (from repo root):
    python3 benchmark/tpch/mia_composition.py [--trials N] [--bootstrap B]
"""

import argparse
import os
import random
import subprocess
import sys

DUCKDB  = "build/release/duckdb"
DB      = "tpch_sf1.db"
PAC_EXT = "build/release/extension/privacy/privacy.duckdb_extension"

PAC_MI  = 1.0 / 128   # MI budget per output cell
DP_EPS  = 0.067       # DP epsilon per query (≈ pac_mi=1/128 equivalent)

PAC_SCHEMA = [
    "PRAGMA clear_privacy_metadata;",
    "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);",
    "ALTER TABLE customer SET PU;",
    "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);",
    "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);",
]

# G=1: plain 2-hop COUNT (full dataset)
BASE_Q = (
    "SELECT COUNT(*) FROM customer"
    " JOIN orders ON c_custkey = o_custkey"
    " JOIN lineitem ON o_orderkey = l_orderkey"
)

K_VALS = [1, 2, 5, 10, 25, 50]


# ── subprocess helpers ────────────────────────────────────────────────────────

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


def run_duckdb_raw(sql):
    proc = subprocess.run(
        [DUCKDB, "-csv", DB, "-c", sql],
        capture_output=True, text=True,
    )
    return [l.strip() for l in proc.stdout.splitlines() if l.strip()]


# ── preambles ─────────────────────────────────────────────────────────────────

def preamble_noiseless():
    return [f"LOAD '{PAC_EXT}';", "SET threads=1;",
            "SET privacy_mode='pac';", "SET privacy_noise=false;"]

def preamble_pac(mi):
    return [f"LOAD '{PAC_EXT}';", "SET threads=1;",
            "SET privacy_mode='pac';", f"SET pac_mi={mi!r};"]

def preamble_dp(epsilon):
    return [f"LOAD '{PAC_EXT}';", "SET threads=1;",
            "SET privacy_mode='dp_elastic';", f"SET dp_epsilon={epsilon!r};", "SET dp_delta=1e-6;"]


# ── core trial runner ─────────────────────────────────────────────────────────

def true_value(query_sql):
    sql = "\n".join(preamble_noiseless() + PAC_SCHEMA + [query_sql + ";"])
    vals = run_duckdb(sql)
    return vals[0] if vals else float("nan")


def run_trials(query_sql, n_trials, pre_lines, batch_size=10):
    """Run n_trials independent noisy queries; return list of floats."""
    results = []
    for start in range(0, n_trials, batch_size):
        n_batch = min(batch_size, n_trials - start)
        seeds = [1337 + (start + i) * 997 for i in range(n_batch)]
        lines = pre_lines + PAC_SCHEMA
        for s in seeds:
            lines.append(f"SET privacy_seed={s};")
            lines.append(query_sql + ";")
        vals = run_duckdb("\n".join(lines))
        results.extend(vals[:n_batch])
    return results[:n_trials]


def llr_scores(y_vals, r_in, r_out):
    return [abs(y - r_out) - abs(y - r_in) for y in y_vals]


# ── bootstrap AUC ─────────────────────────────────────────────────────────────

def bootstrap_auc(scores_in, scores_out, k, n_bootstrap, rng_seed=42):
    rng = random.Random(rng_seed)
    wins = 0.0
    for _ in range(n_bootstrap):
        si = sum(rng.choices(scores_in,  k=k))
        so = sum(rng.choices(scores_out, k=k))
        wins += 1.0 if si > so else (0.5 if si == so else 0.0)
    return wins / n_bootstrap


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trials",    type=int, default=200)
    ap.add_argument("--bootstrap", type=int, default=2000)
    args = ap.parse_args()

    for path in [DB, DUCKDB, PAC_EXT]:
        if not os.path.exists(path):
            sys.exit(f"Not found: {path}")

    # ── Find target customer ─────────────────────────────────────────────────
    print("Finding target customer...", end=" ", flush=True)
    t_vals = run_duckdb(
        "PRAGMA clear_privacy_metadata;\n"
        "SELECT o_custkey FROM orders GROUP BY o_custkey ORDER BY COUNT(*) DESC LIMIT 1;\n"
    )
    target = int(t_vals[0])

    # Find which order-year has the most of the target's lineitems
    year_rows = run_duckdb_raw(
        f"LOAD '{PAC_EXT}';\nSET privacy_mode='pac';\nSET privacy_noise=false;\n"
        + "\n".join(PAC_SCHEMA) + "\n"
        + f"SELECT EXTRACT(YEAR FROM o_orderdate)::INT AS yr, COUNT(*) AS cnt"
          f" FROM customer JOIN orders ON c_custkey=o_custkey"
          f" JOIN lineitem ON o_orderkey=l_orderkey"
          f" WHERE c_custkey={target}"
          f" GROUP BY yr ORDER BY cnt DESC LIMIT 1;\n"
    )
    # Skip "success"/"Note:" lines from DDL; find the first data row (two comma-separated ints)
    target_year, target_year_delta = 1994, 0
    for row in year_rows:
        parts = row.split(",")
        if len(parts) == 2:
            try:
                target_year = int(parts[0])
                target_year_delta = int(parts[1])
                break
            except ValueError:
                pass

    print(f"customer {target}  (peak year={target_year}, Δ_year={target_year_delta})")

    # ── Build query variants ─────────────────────────────────────────────────
    #
    # G=1: single global COUNT.  Full Δ=158 in one cell.
    # G=7: GROUP BY order year.  Adversary extracts target's peak year cell.
    #      PAC noises all 7 cells → costs 7 × mi per query.
    #      DP  noises all 7 cells → costs ε per query (global sensitivity: one
    #          customer affects multiple year-cells, but dp_elastic charges ε total).

    q1_in  = BASE_Q
    q1_out = BASE_Q + f" WHERE c_custkey != {target}"

    q7_in  = (
        f"SELECT cnt FROM ("
        f"SELECT EXTRACT(YEAR FROM o_orderdate)::INT AS yr, COUNT(*) AS cnt"
        f" FROM customer JOIN orders ON c_custkey=o_custkey"
        f" JOIN lineitem ON o_orderkey=l_orderkey"
        f" GROUP BY yr) WHERE yr = {target_year}"
    )
    q7_out = (
        f"SELECT cnt FROM ("
        f"SELECT EXTRACT(YEAR FROM o_orderdate)::INT AS yr, COUNT(*) AS cnt"
        f" FROM customer JOIN orders ON c_custkey=o_custkey"
        f" JOIN lineitem ON o_orderkey=l_orderkey"
        f" WHERE c_custkey != {target}"
        f" GROUP BY yr) WHERE yr = {target_year}"
    )

    # ── True values ──────────────────────────────────────────────────────────
    print("\nTrue values and Δ:")
    r_in_1  = true_value(q1_in);  r_out_1  = true_value(q1_out)
    r_in_7  = true_value(q7_in);  r_out_7  = true_value(q7_out)
    print(f"  G=1 (global COUNT):         Δ = {r_in_1 - r_out_1:.0f}")
    print(f"  G=7 (year={target_year}):              Δ = {r_in_7 - r_out_7:.0f}  "
          f"(target's lineitems in {target_year})")

    # ── Collect trial scores ─────────────────────────────────────────────────
    MODES = [
        ("PAC G=1", preamble_pac(PAC_MI), q1_in, q1_out, r_in_1,  r_out_1,  1,  PAC_MI),
        ("DP  G=1", preamble_dp(DP_EPS),  q1_in, q1_out, r_in_1,  r_out_1,  1,  DP_EPS),
        ("PAC G=7", preamble_pac(PAC_MI), q7_in, q7_out, r_in_7,  r_out_7,  7,  PAC_MI),
        ("DP  G=7", preamble_dp(DP_EPS),  q7_in, q7_out, r_in_7,  r_out_7,  7,  DP_EPS),
    ]
    # budget_per_query = G * mi  for PAC,  ε  for DP  (regardless of G)
    budget_per_query = {
        "PAC G=1": 1 * PAC_MI,
        "DP  G=1": DP_EPS,
        "PAC G=7": 7 * PAC_MI,
        "DP  G=7": DP_EPS,
    }

    print(f"\nRunning {args.trials} trials × 4 modes × 2 worlds...")
    all_scores = {}
    for label, pre, q_in, q_out, r_in, r_out, g, param in MODES:
        print(f"  [{label}] IN...", end=" ", flush=True)
        y_in  = run_trials(q_in,  args.trials, list(pre))
        print("OUT...", end=" ", flush=True)
        y_out = run_trials(q_out, args.trials, list(pre))
        sc_in  = llr_scores(y_in,  r_in, r_out)
        sc_out = llr_scores(y_out, r_in, r_out)
        mean_in  = sum(sc_in)  / len(sc_in)  if sc_in  else float("nan")
        mean_out = sum(sc_out) / len(sc_out) if sc_out else float("nan")
        print(f"done  (mean IN={mean_in:+.1f}  OUT={mean_out:+.1f})")
        all_scores[label] = (sc_in, sc_out)

    # ── Table 1: AUC vs K (queries issued) ───────────────────────────────────
    print()
    print("Table 1 — AUC vs K (queries issued)")
    print("PAC and DP use the same noise distribution per cell → similar AUC at same K.")
    print("G=7 has lower AUC because target's signal is split across years (Δ=158→39).")
    print()
    col_w = 10
    labels = [m[0] for m in MODES]
    hdr = f"{'K':>5}" + "".join(f"  {l:>{col_w}}" for l in labels)
    print(hdr)
    print("─" * len(hdr))
    for k in K_VALS:
        if k > args.trials:
            break
        row = f"{k:>5}"
        for label in labels:
            sc_in, sc_out = all_scores[label]
            auc = bootstrap_auc(sc_in, sc_out, k, args.bootstrap)
            row += f"  {auc:>{col_w}.4f}"
        print(row)

    # ── Table 2: AUC vs formal budget consumed ───────────────────────────────
    print()
    print("Table 2 — AUC vs formal budget consumed per K queries")
    print("Budget = K × G × mi  for PAC  |  K × ε  for DP  (ε does not scale with G)")
    print("At the same budget, AUC should be similar regardless of mechanism.")
    print()

    # Gather all unique budget points from K × cost_per_query for each mode
    budget_points = sorted(set(
        round(k * budget_per_query[label], 6)
        for label in labels
        for k in K_VALS
        if k <= args.trials
    ))

    hdr2 = f"{'budget':>8}" + "".join(f"  {l:>{col_w}}" for l in labels)
    print(hdr2)
    print("─" * len(hdr2))
    for b in budget_points:
        row = f"{b:>8.4f}"
        for label in labels:
            cost = budget_per_query[label]
            k = max(1, round(b / cost))
            if k > args.trials:
                row += f"  {'N/A':>{col_w}}"
            else:
                sc_in, sc_out = all_scores[label]
                auc = bootstrap_auc(sc_in, sc_out, k, args.bootstrap)
                row += f"  {auc:>{col_w}.4f}"
        print(row)

    # ── Budget summary ────────────────────────────────────────────────────────
    print()
    print("Budget accounting summary  (pac_mi=1/128, dp_ε=0.067 ≈ equivalent per-cell privacy)")
    print()
    print(f"  {'Mode':<10}  {'cost/query':>12}  {'queries at B=0.39':>18}  {'queries at B=3.35':>18}")
    print(f"  {'────':<10}  {'──────────':>12}  {'──────────────────':>18}  {'──────────────────':>18}")
    for label in labels:
        cost = budget_per_query[label]
        unit = "nats(MI)" if "PAC" in label else "nats(ε) "
        q039 = round(0.39 / cost)
        q335 = round(3.35 / cost)
        print(f"  {label:<10}  {cost:>10.4f} {unit}  {q039:>18}  {q335:>18}")
    print()
    print("  Note: B=0.39 = 50 × PAC-G=1 queries;  B=3.35 = 50 × DP-G=1 queries")
    print("  DP charges ε per query regardless of G → GROUP BY does not multiply budget")
    print("  PAC charges G × mi per query          → GROUP BY multiplies budget by G")
    print()
    print("  At G=7: PAC cost = 7/128 = 0.055,  DP cost = 0.067 → roughly equivalent")
    print("  At G=25: PAC cost = 25/128 = 0.195, DP cost = 0.067 → PAC 3× more expensive")

    print()
    print("Notes:")
    print("  LLR score: |y - r_out| - |y - r_in|  (positive → predict IN)")
    print("  G=7 adversary attacks only the target's peak-year cell; other 6 cells unused")
    print(f"  pac_mi=1/128 ≈ dp_ε=0.067  (Sridhar thesis Table 3.2)")


if __name__ == "__main__":
    main()
