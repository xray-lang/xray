import unittest

from run import RATIO_LIMITS
from array_access_benchmark import NATIVE_LIMITS, balanced_block_ratios, lower_quartile


class LayoutRatioTests(unittest.TestCase):
    def test_independent_percentiles_can_select_different_load_periods(self):
        managed = [64.2, 57.3, 56.0, 69.5, 72.5, 63.2, 74.5, 67.1, 62.3, 47.9]
        raw = [47.6, 58.4, 57.9, 44.0, 44.0, 53.5, 46.6, 52.5, 41.9, 40.5]
        limit = RATIO_LIMITS["map-take"]

        self.assertGreater(lower_quartile(managed) / lower_quartile(raw), limit)
        self.assertAlmostEqual(lower_quartile(balanced_block_ratios(managed, raw)), 1.232, places=3)

    def test_a_slow_raw_block_does_not_mask_a_sustained_regression(self):
        managed = [134.0] * 12
        raw = [200.0, 200.0] + [100.0] * 10
        self.assertGreater(lower_quartile(balanced_block_ratios(managed, raw)), RATIO_LIMITS["map-take"])

    def test_array_replay_uses_the_same_paired_estimator(self):
        native = [37.5, 35.4, 33.4, 29.7, 27.7, 32.0, 45.7, 44.2, 37.7]
        raw = [38.7, 27.2, 27.7, 28.0, 34.5, 29.5, 29.3, 35.8, 32.1]
        limit = NATIVE_LIMITS["inplace"]

        self.assertGreater(lower_quartile(native) / lower_quartile(raw), limit)
        self.assertLess(lower_quartile(balanced_block_ratios(native[:-1], raw[:-1])), limit)

    def test_incomplete_pair_is_rejected(self):
        with self.assertRaises(ValueError):
            balanced_block_ratios([1.0] * 11, [1.0] * 11)


if __name__ == "__main__":
    unittest.main()
