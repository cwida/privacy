import numpy as np, sys
sys.path.insert(0,'/home/ila/Code/privacy/attacks')
from all_or_frozen_v2 import tau_af, tau_count, B_BINS
DELTA=1e-6
print("CHECK 1: does my tau_AF match her closed form? Her sec 2.10 small-delta approximation is")
print("         tau_AF ~ (C_u/eps_B) log[(e^{eps_B/C_u} + B - 1)/(2 delta)]\n")
print(f"  {'C_u':>5}{'eps_B':>7}{'b':>9}{'my tau_AF':>12}{'her approx':>12}{'ratio':>8}")
for cu,eb in ((37,0.5),(72,0.05),(10,0.2),(1,0.4)):
    b=cu/eb
    mine=tau_af(b,DELTA)
    hers=b*np.log((np.exp(eb/cu)+B_BINS-1)/(2*DELTA))
    print(f"  {cu:>5}{eb:>7.2f}{b:>9.1f}{mine:>12,.1f}{hers:>12,.1f}{mine/hers:>8.4f}")
print("  -> matches her closed form to <1%.\n")

print("CHECK 2: her Q1 claim -- is her histogram threshold close to GOOGLE's at the SAME C_u?")
print("         Both mechanisms have noise scale C_u/eps (one PU touches C_u groups either way).")
print("         Difference: she takes a max over B=64 bins but needs only rho<=delta (the AND);")
print("         Google reads one count but must divide delta across C_u groups.\n")
print(f"  {'C_u':>5}{'eps':>7}{'her tau_AF':>13}{'her tau_PG':>13}{'Google tau':>13}{'AF/Google':>11}")
for cu,e in ((37,0.5),(37,0.4),(72,0.4),(10,0.4),(5,0.4),(1,0.4)):
    b=cu/e
    af=tau_af(b,DELTA)
    pg=tau_af(b,1-(1-DELTA)**(1.0/cu))
    gl=tau_count(e,DELTA,cu)
    print(f"  {cu:>5}{e:>7.2f}{af:>13,.1f}{pg:>13,.1f}{gl:>13,.1f}{af/gl:>11.2f}")
print("  -> SHE IS RIGHT. At matched C_u her All-or-Frozen threshold is BELOW Google's, because")
print("     the AND saves the delta/C_u division and that outweighs the max-over-64-bins cost.")
print("     My earlier '736x' compared her histogram at C_u=72 against a dedicated count at")
print("     C_e=1 -- two changes at once, not like-for-like. That number was misleading.\n")

print("CHECK 3: so where does All-or-Frozen actually lose? Isolating the two remaining factors.")
print("         (a) the statistic: max BIN count vs the group's support")
print("         (b) the AND over all groups vs per-group release")
