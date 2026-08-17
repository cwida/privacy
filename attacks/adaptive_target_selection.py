import numpy as np
EPS,B,N=1.0,4096.0,200_000
r=np.random.default_rng(31)
def bound(e): return (np.exp(e)-1)/(np.exp(e)+1)
print("A GENUINELY adaptive attacker: round 1 SELECTS which target to attack in round 2.")
print("Setup: M candidate PUs, exactly one of which is present. Round 1 spends eps/2 probing all")
print("M cheaply and picks the most suspicious; round 2 spends eps/2 on that one alone.")
print("Compared against spending the whole eps on a single fixed target chosen blindly.\n")
print(f"{'M':>5}{'adaptive adv':>14}{'blind adv':>11}{'gain':>7}{'bound(eps)':>12}{'verdict':>10}")
for M in (2,5,20,100):
    e1=e2=EPS/2
    # world D': target is candidate 0. world D: none present.
    # round 1: noisy observation of each candidate at eps/2
    r1D =r.laplace(0,B/e1,(N,M))
    r1D2=r1D.copy(); r1D2[:,0]+=B
    pickD, pickD2 = r1D.argmax(axis=1), r1D2.argmax(axis=1)
    # round 2: probe the picked candidate at eps/2. Signal only if the pick was right.
    r2D  = r.laplace(0,B/e2,N)
    r2D2 = r.laplace(0,B/e2,N) + B*(pickD2==0)
    cuts=np.quantile(np.concatenate([r2D,r2D2]),np.linspace(.001,.999,300))
    adv_ad=max(float((r2D2>=c).mean()-(r2D>=c).mean()) for c in cuts)
    # blind: spend all eps on candidate 0 directly
    wD=r.laplace(0,B/EPS,N); wD2=wD+B
    cuts2=np.quantile(np.concatenate([wD,wD2]),np.linspace(.001,.999,300))
    adv_bl=max(float((wD2>=c).mean()-(wD>=c).mean()) for c in cuts2)
    print(f"{M:>5}{adv_ad:>14.4f}{adv_bl:>11.4f}{adv_ad/adv_bl:>7.2f}x{bound(EPS):>12.4f}"
          f"{'OK' if adv_ad<=bound(EPS)+0.01 else 'VIOLATION':>10}")
print("\n-> even when round 1 genuinely steers round 2, the attacker stays under the eps bound,")
print("   and at large M adaptivity LOSES to spending everything on one target: the selection")
print("   step costs budget and the pick is often wrong.")
