import itertools
import unittest

import numpy as np

from automatic_cu_future import (
    em_probabilities,
    geometric_candidates,
    hybrid_candidates,
    rank_scores,
)


class AutomaticCuFutureTest(unittest.TestCase):
    def test_direct_rank_score_sensitivity(self):
        for p in (0.3, 0.7, 0.95, 0.99):
            sensitivity = max(p, 1.0 - p)
            for size in range(1, 6):
                for data in itertools.product(range(1, 5), repeat=size):
                    data = np.asarray(data)
                    for removed in range(size):
                        neighbor = np.delete(data, removed)
                        delta = np.max(
                            np.abs(
                                rank_scores(data, np.arange(1, 5), p)
                                - rank_scores(neighbor, np.arange(1, 5), p)
                            )
                        )
                        self.assertLessEqual(delta, sensitivity + 1e-12)

    def test_geometric_candidates_depend_only_on_public_bound(self):
        candidates = geometric_candidates(2095)
        self.assertEqual(candidates[0], 1)
        self.assertEqual(candidates[-1], 2095)
        self.assertTrue(np.all(np.diff(candidates) > 0))
        self.assertLess(len(candidates), 30)

        hybrid = hybrid_candidates(2095)
        self.assertEqual(hybrid[0], 1)
        self.assertEqual(hybrid[-1], 2095)
        self.assertTrue(np.all(np.diff(hybrid) > 0))
        self.assertTrue(np.array_equal(hybrid[:128], np.arange(1, 129)))

    def test_lattice_reduces_upper_tail_multiplicity(self):
        k_u = np.repeat([1, 2], 500)
        public_k = 10_000
        observed_max = 2
        p = 0.95
        epsilon = 0.1
        sensitivity = max(p, 1.0 - p)

        dense = np.arange(1, public_k + 1)
        lattice = geometric_candidates(public_k)
        dense_probability = em_probabilities(
            rank_scores(k_u, dense, p), epsilon, sensitivity
        )
        lattice_probability = em_probabilities(
            rank_scores(k_u, lattice, p), epsilon, sensitivity
        )
        dense_tail = dense_probability[dense > observed_max].sum()
        lattice_tail = lattice_probability[lattice > observed_max].sum()
        self.assertLess(lattice_tail, dense_tail)


if __name__ == "__main__":
    unittest.main()
