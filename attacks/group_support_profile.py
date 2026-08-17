import sys; sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import numpy as np, duckdb
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
for al,p in (("tpch","tpch_sass_sf10.db"),("so","stackoverflow_dba_sqlstorm.db"),
             ("cb","clickbench_micro.db")):
    con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
print("Q(2): how common are low-support groups? Support = distinct PUs in a group.")
print("tau at C_e=1, eps_eta=0.4, delta=1e-6 is ~34, so a group under ~34 can never pass.\n")
print(f"{'query':<28}{'groups':>8}{'min':>6}{'p1':>7}{'p5':>7}{'median':>9}"
      f"{'<10':>7}{'<34':>7}{'<100':>7}")
print("-"*86)
QS={
 "tpch price mo|nation":"""SELECT strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar) g,
   count(DISTINCT o_custkey) n FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
   JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=8000 GROUP BY 1""",
 "tpch price month":"""SELECT strftime(l_shipdate,'%Y-%m') g, count(DISTINCT o_custkey) n
   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
   JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=8000 GROUP BY 1""",
 "tpch price day":"""SELECT cast(l_shipdate as varchar) g, count(DISTINCT o_custkey) n
   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
   JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=9500 GROUP BY 1""",
 "tpch price mo|prio":"""SELECT strftime(l_shipdate,'%Y-%m')||'|'||o_orderpriority g,
   count(DISTINCT o_custkey) n FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
   JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=9500 GROUP BY 1""",
 "so posts month":"""SELECT strftime(CreationDate,'%Y-%m') g, count(DISTINCT OwnerUserId) n
   FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1""",
 "so posts day":"""SELECT cast(cast(CreationDate as date) as varchar) g,
   count(DISTINCT OwnerUserId) n FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1""",
 "so comments month":"""SELECT strftime(CreationDate,'%Y-%m') g, count(DISTINCT UserId) n
   FROM so.Comments WHERE UserId IS NOT NULL GROUP BY 1""",
 "cb hits region":"""SELECT cast(RegionID as varchar) g, count(DISTINCT UserID) n
   FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1""",
 "cb hits date|region":"""SELECT cast(EventDate as varchar)||'|'||cast(RegionID as varchar) g,
   count(DISTINCT UserID) n FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1""",
}
for name,q in QS.items():
    n=np.array([x[1] for x in con.execute(q).fetchall()],float)
    print(f"{name:<28}{len(n):>8,}{n.min():>6.0f}{np.percentile(n,1):>7.0f}"
          f"{np.percentile(n,5):>7.0f}{np.median(n):>9,.0f}"
          f"{100*np.mean(n<10):>6.0f}%{100*np.mean(n<34):>6.0f}%{100*np.mean(n<100):>6.0f}%")
