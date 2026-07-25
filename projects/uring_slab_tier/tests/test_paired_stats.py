from __future__ import annotations

import unittest

from uring_slab_tier.paired_stats import (
    balanced_pair_orders,
    evaluate_aa_campaign,
)


def _gate(required_pairs: int) -> dict:
    return {
        "required_complete_pairs": required_pairs,
        "primary_metric": "load_sustained_throughput_mib_s",
        "higher_is_better": True,
        "bootstrap_samples": 500,
        "bootstrap_seed": 240,
        "sign_flip_samples": 2_000,
        "sign_flip_seed": 241,
        "power_samples": 2_000,
        "power_seed": 242,
        "minimum_detectable_effect": 0.25,
        "pass_absolute_aa_bias": 0.05,
        "max_absolute_aa_bias": 0.10,
        "pass_noise_floor": 0.10,
        "max_noise_floor": 0.15,
        "minimum_power": 0.80,
        "pass_absolute_window_effect": 0.10,
        "max_absolute_window_effect": 0.20,
        "pass_absolute_position_effect": 0.05,
        "max_absolute_position_effect": 0.10,
    }


def _pair(
    index: int,
    value_a: float,
    value_b: float,
    *,
    status: str = "PASS",
) -> dict:
    return {
        "pair_id": f"p{index:02d}",
        "window": index // 10,
        "load_order": "AB" if index % 2 == 0 else "BA",
        "status": status,
        "run_a": {
            "status": status,
            "metrics": {"load_sustained_throughput_mib_s": value_a},
        },
        "run_b": {
            "status": status,
            "metrics": {"load_sustained_throughput_mib_s": value_b},
        },
    }


class PairedStatsTest(unittest.TestCase):
    def test_balanced_orders_are_deterministic_and_balanced(self) -> None:
        first = balanced_pair_orders(windows=3, pairs_per_window=10, seed=240)
        second = balanced_pair_orders(windows=3, pairs_per_window=10, seed=240)
        self.assertEqual(first, second)
        for window in range(3):
            orders = [
                item["order"] for item in first if item["window"] == window
            ]
            self.assertEqual(orders.count("AB"), 5)
            self.assertEqual(orders.count("BA"), 5)

    def test_odd_pairs_per_window_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "even"):
            balanced_pair_orders(windows=1, pairs_per_window=3, seed=1)

    def test_stable_aa_campaign_passes(self) -> None:
        pairs = [_pair(index, 1000.0, 1000.0) for index in range(30)]
        result = evaluate_aa_campaign(pairs, _gate(30))
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(
            result["metrics"]["sign_flip_paired_median_noise_floor_95"],
            0.0,
        )
        self.assertEqual(
            result["criteria"]["minimum_effect_detection_power"], "PASS"
        )

    def test_noisy_campaign_fails_noise_and_power(self) -> None:
        ratios = [0.45, 1.80, 0.55, 1.65, 0.60, 1.50] * 5
        pairs = [
            _pair(index, 1000.0, 1000.0 * ratio)
            for index, ratio in enumerate(ratios)
        ]
        result = evaluate_aa_campaign(pairs, _gate(30))
        self.assertEqual(result["status"], "FAIL")
        self.assertIn(
            "FAIL",
            {
                result["criteria"]["paired_median_noise_floor"],
                result["criteria"]["minimum_effect_detection_power"],
            },
        )

    def test_incomplete_fixed_campaign_fails_without_replacement(self) -> None:
        pairs = [_pair(index, 1000.0, 1000.0) for index in range(29)]
        result = evaluate_aa_campaign(pairs, _gate(30))
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["reason"], "incomplete_fixed_campaign")


if __name__ == "__main__":
    unittest.main()
