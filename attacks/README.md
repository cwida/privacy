# Filterless privacy attacks

`filterless_attacks.py` is the single maintained Python harness. It reproduces the filter-change
and sampled-PU add/remove knife edges described in `docs/dp/filterless_encoded_pu.md`:

```bash
python3 attacks/filterless_attacks.py
```

The first experiment is a same-database, different-predicate channel diagnostic. The second uses
add/remove adjacency and compares the removed raw-histogram control with the current private
histogram model. The script prints the pure-DP equal-prior membership-accuracy ceiling for the
configured total epsilon; only the neighboring-database private-histogram result is compared with
that ceiling.

Older filenames in the chronological design notes identify historical exploratory harnesses. Those
harnesses were removed rather than retained as unsupported or superseded attack implementations.
