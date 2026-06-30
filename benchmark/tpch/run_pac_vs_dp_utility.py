#!/usr/bin/env python3
"""
PAC vs dp_elastic utility microbenchmark on TPC-H SF1.

Runs 10 aggregate queries under both privacy modes, computes mean absolute
relative error (%) against exact query results across N random seeds.

Usage (from repo root):
    python3 benchmark/tpch/run_pac_vs_dp_utility.py [--runs N] [--epsilons E1 E2 ...] [--pac-mi M]
"""

import argparse
import os
import statistics
import subprocess
import sys

DUCKDB = "build/release/duckdb"
DB = "tpch_sf1.db"
PAC_EXT = "build/release/extension/privacy/privacy.duckdb_extension"

# (label, sql, dp_sum_bound_or_None)
# FK chain: lineitem → orders → customer (PU)
QUERIES = [
    # ── Single table (PU only) ─────────────────────────────────────────────
    ("Q01 COUNT(customer)        ",
     "SELECT COUNT(*) FROM customer",
     None),
    ("Q02 SUM(c_acctbal)         ",
     "SELECT SUM(c_acctbal) FROM customer",
     10000.0),   # c_acctbal ∈ [-999.99, 9999.99]

    # ── 1-hop join (orders → customer) ────────────────────────────────────
    ("Q03 COUNT(orders 1-hop)    ",
     "SELECT COUNT(*) FROM customer JOIN orders ON c_custkey = o_custkey",
     None),
    ("Q04 SUM(o_totalprice 1-hop)",
     "SELECT SUM(o_totalprice) FROM customer JOIN orders ON c_custkey = o_custkey",
     600000.0),  # o_totalprice ∈ [800, ~560000]

    # ── 2-hop join (lineitem → orders → customer) ──────────────────────────
    ("Q05 COUNT(lineitem 2-hop)  ",
     "SELECT COUNT(*) FROM customer JOIN orders ON c_custkey = o_custkey"
     " JOIN lineitem ON o_orderkey = l_orderkey",
     None),
    ("Q06 SUM(l_quantity 2-hop)  ",
     "SELECT SUM(l_quantity) FROM customer JOIN orders ON c_custkey = o_custkey"
     " JOIN lineitem ON o_orderkey = l_orderkey",
     50.0),      # l_quantity ∈ [1, 50]
    ("Q07 SUM(l_extprice 2-hop)  ",
     "SELECT SUM(l_extendedprice) FROM customer JOIN orders ON c_custkey = o_custkey"
     " JOIN lineitem ON o_orderkey = l_orderkey",
     105000.0),  # l_extendedprice ∈ [900, ~104050]
    ("Q08 SUM(l_discount 2-hop)  ",
     "SELECT SUM(l_discount) FROM customer JOIN orders ON c_custkey = o_custkey"
     " JOIN lineitem ON o_orderkey = l_orderkey",
     0.1),       # l_discount ∈ [0.00, 0.10]
    ("Q09 SUM(l_tax 2-hop)       ",
     "SELECT SUM(l_tax) FROM customer JOIN orders ON c_custkey = o_custkey"
     " JOIN lineitem ON o_orderkey = l_orderkey",
     0.08),      # l_tax ∈ [0.00, 0.08]
    ("Q10 SUM(l_linenumber 2-hop)",
     "SELECT SUM(l_linenumber) FROM customer JOIN orders ON c_custkey = o_custkey"
     " JOIN lineitem ON o_orderkey = l_orderkey",
     7.0),       # l_linenumber ∈ [1, 7]
]


PAC_SCHEMA = [
    "PRAGMA clear_privacy_metadata;",
    "ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);",
    "ALTER TABLE customer SET PU;",
    "ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);",
    "ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);",
]


def build_session_sql(preamble_lines, queries_with_bounds, mode_label):
    lines = preamble_lines[:]
    lines.extend(PAC_SCHEMA)
    for sql, sum_bound in queries_with_bounds:
        if "dp_elastic" in "\n".join(preamble_lines):
            if sum_bound is not None:
                lines.append(f"SET dp_sum_bound={sum_bound};")
            else:
                lines.append("RESET dp_sum_bound;")
        lines.append(sql + ";")
    return "\n".join(lines)


def run_duckdb(sql_script):
    """Execute sql_script; return (float_values, returncode, stderr)."""
    proc = subprocess.run(
        [DUCKDB, "-csv", DB, "-c", sql_script],
        capture_output=True,
        text=True,
    )
    values = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            values.append(float(line))
        except ValueError:
            pass  # header line
    return values, proc.returncode, proc.stderr.strip()


def get_true_values():
    preamble = [
        f"LOAD '{PAC_EXT}';",
        "SET threads=1;",
        "SET privacy_mode='pac';",
        "SET privacy_noise=false;",
    ]
    sql = build_session_sql(preamble, [(sql, None) for _, sql, _ in QUERIES], "true")
    vals, _, _ = run_duckdb(sql)
    return vals


def get_mf_k():
    sql = (
        f"LOAD '{PAC_EXT}';\n"
        "SET threads=1; SET privacy_mode='pac'; SET privacy_noise=false;\n"
        "SELECT MAX(cnt) FROM (SELECT COUNT(*) AS cnt FROM orders GROUP BY o_custkey);\n"
        "SELECT MAX(cnt) FROM (SELECT COUNT(*) AS cnt FROM lineitem GROUP BY l_orderkey);\n"
    )
    vals, _, _ = run_duckdb(sql)
    return (vals[0] if len(vals) > 0 else 0.0,
            vals[1] if len(vals) > 1 else 0.0)


def build_pac_sql(pac_mi, seed):
    preamble = [
        f"LOAD '{PAC_EXT}';",
        "SET threads=1;",
        f"SET privacy_seed={seed};",
        "SET privacy_mode='pac';",
        f"SET pac_mi={pac_mi!r};",
    ]
    return build_session_sql(preamble, [(sql, sb) for _, sql, sb in QUERIES], "pac")


def build_dp_sql(epsilon, seed):
    preamble = [
        f"LOAD '{PAC_EXT}';",
        "SET threads=1;",
        f"SET privacy_seed={seed};",
        "SET privacy_mode='dp_elastic';",
        f"SET dp_epsilon={epsilon!r};",
        "SET dp_delta=1e-6;",
    ]
    return build_session_sql(preamble, [(sql, sb) for _, sql, sb in QUERIES], "dp_elastic")


def rel_error_pct(noised, true_val):
    if abs(true_val) < 1e-12:
        return abs(noised) * 100.0
    return abs(noised - true_val) / abs(true_val) * 100.0


def fmt_err(x):
    if x != x:
        return "    N/A"
    if x < 0.001:
        return f"{x:8.5f}%"
    if x < 10:
        return f"{x:8.4f}%"
    return f"{x:8.2f}%"


def fmt_tv(x):
    if abs(x) >= 1e9:
        return f"{x / 1e9:10.3f}B"
    if abs(x) >= 1e6:
        return f"{x / 1e6:10.3f}M"
    if abs(x) >= 1e3:
        return f"{x / 1e3:10.3f}K"
    return f"{x:13.4f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", type=int, default=30)
    ap.add_argument("--epsilons", type=float, nargs="+", default=[0.01, 0.05, 0.1, 0.5, 1.0])
    ap.add_argument("--pac-mi", type=float, default=1.0/128)
    args = ap.parse_args()

    if not os.path.exists(DB):
        sys.exit(f"Error: {DB} not found — run from repo root.")
    if not os.path.exists(DUCKDB):
        sys.exit(f"Error: {DUCKDB} not found — build first with: GEN=ninja make")

    n_q = len(QUERIES)
    epsilons = args.epsilons

    # ── mf_K / ES info ──────────────────────────────────────────────────────
    print("Computing mf_K values...", end=" ", flush=True)
    mf_orders, mf_lineitem = get_mf_k()
    es = [1.0, 1.0, mf_orders, mf_orders,
          mf_orders * mf_lineitem, mf_orders * mf_lineitem,
          mf_orders * mf_lineitem, mf_orders * mf_lineitem,
          mf_orders * mf_lineitem, mf_orders * mf_lineitem]
    print(f"mf_K(orders)={mf_orders:.0f}  mf_K(lineitem)={mf_lineitem:.0f}")
    print(f"Elastic sensitivity: 1-hop ES={mf_orders:.0f}   2-hop ES={mf_orders*mf_lineitem:.0f}")
    print()

    # ── True values ─────────────────────────────────────────────────────────
    print("Computing true values...", end=" ", flush=True)
    true_vals = get_true_values()
    print("done")
    print()

    # ── Benchmark runs ───────────────────────────────────────────────────────
    pac_errors = [[] for _ in range(n_q)]
    dp_errors  = [[[] for _ in range(n_q)] for _ in epsilons]

    total_runs = args.runs * (1 + len(epsilons))
    print(f"Running {args.runs} seeds × (1 PAC + {len(epsilons)} DP) × {n_q} queries  "
          f"[pac_mi={args.pac_mi:.6g}, epsilons={epsilons}]", flush=True)

    for run_idx in range(args.runs):
        seed = (run_idx + 1) * 997
        sys.stdout.write(f"\r  seed {run_idx + 1}/{args.runs}")
        sys.stdout.flush()

        pac_vals, _, _ = run_duckdb(build_pac_sql(args.pac_mi, seed))
        for i, v in enumerate(pac_vals[:n_q]):
            if i < len(true_vals):
                pac_errors[i].append(rel_error_pct(v, true_vals[i]))

        for e_idx, eps in enumerate(epsilons):
            dp_vals, _, dp_err = run_duckdb(build_dp_sql(eps, seed))
            if len(dp_vals) < n_q and run_idx == 0:
                print(f"\n  [warn] DP ε={eps} seed {seed}: {len(dp_vals)}/{n_q} results — {dp_err[:80]}",
                      file=sys.stderr)
            for i, v in enumerate(dp_vals[:n_q]):
                if i < len(true_vals):
                    dp_errors[e_idx][i].append(rel_error_pct(v, true_vals[i]))

    print("\r" + " " * 60 + "\r", end="")

    # ── Results table ────────────────────────────────────────────────────────
    eps_headers = "  ".join(f"DP ε={e:<5g}" for e in epsilons)
    print(f"PAC vs dp_elastic — TPC-H SF1 Utility (mean |rel. error|%)")
    print(f"PAC: pac_mi={args.pac_mi:.6g}   runs={args.runs}")
    print()

    col_w = 10  # width per DP epsilon column
    hdr = f"{'Query':<33} {'True value':>13}  {'PAC err%':>9}  " + \
          "  ".join(f"{'DP ε='+str(e):>9}" for e in epsilons)
    print(hdr)
    print("─" * len(hdr))

    for i, (label, sql, sum_bound) in enumerate(QUERIES):
        tv = true_vals[i] if i < len(true_vals) else float("nan")
        pe = pac_errors[i]
        pac_mean = statistics.mean(pe) if pe else float("nan")

        dp_means = []
        for e_idx in range(len(epsilons)):
            de = dp_errors[e_idx][i]
            dp_means.append(statistics.mean(de) if de else float("nan"))

        tv_str = fmt_tv(tv) if tv == tv else "          N/A"
        dp_cols = "  ".join(fmt_err(dm) for dm in dp_means)
        print(f"{label:<33} {tv_str:>13}  {fmt_err(pac_mean):>9}  {dp_cols}")

    print()
    print("Notes:")
    print(f"  Utility = mean |noised − true| / |true| × 100%")
    print(f"  PAC noise calibrated to pac_mi={args.pac_mi:.6g} nats mutual information per query")
    print(f"  DP Laplace scale = ES × C / ε  (C = dp_sum_bound for SUM, 1 for COUNT)")
    print(f"  PAC equivalent DP budget: pac_mi=1/128 nats ≈ ε ≈ 0.067  (from Sridhar thesis Table 3.2)")


if __name__ == "__main__":
    main()
