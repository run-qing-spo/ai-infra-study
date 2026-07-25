from __future__ import annotations

import unittest

from uring_slab_tier.noise_gate import evaluate_campaign


def gate_config(minimum_valid_runs: int = 6) -> dict:
    return {
        "minimum_valid_runs": minimum_valid_runs,
        "bootstrap_samples": 200,
        "bootstrap_seed": 1,
        "throughput_robust_cv_pass": 0.15,
        "throughput_robust_cv_fail": 0.30,
        "throughput_relative_ci_halfwidth_pass": 0.10,
        "throughput_relative_ci_halfwidth_fail": 0.20,
        "throughput_window_ratio_pass": 1.25,
        "throughput_window_ratio_fail": 1.50,
        "sojourn_p95_robust_cv_pass": 0.25,
        "sojourn_p95_robust_cv_fail": 0.50,
        "sojourn_p95_relative_ci_halfwidth_pass": 0.20,
        "sojourn_p95_relative_ci_halfwidth_fail": 0.40,
        "sojourn_p95_window_ratio_pass": 1.50,
        "sojourn_p95_window_ratio_fail": 2.00,
    }


def run_result(run_id: str, window: int, scale: float = 1.0) -> dict:
    return {
        "run_id": run_id,
        "window": window,
        "status": "PASS",
        "correctness": {"pass": True},
        "metrics": {
            "store_throughput_mib_s": 1000.0 * scale,
            "load_throughput_mib_s": 2000.0 * scale,
            "store_job_p95_ms": 10.0 / scale,
            "load_job_p95_ms": 5.0 / scale,
        },
    }


class NoiseGateTest(unittest.TestCase):
    def test_stable_campaign_passes(self) -> None:
        results = [
            run_result(f"r{index}", index // 2, 1.0 + (index % 2) * 0.01)
            for index in range(6)
        ]
        self.assertEqual(
            evaluate_campaign(results, gate_config())["status"], "PASS"
        )

    def test_insufficient_runs_fail(self) -> None:
        results = [run_result("r0", 0)]
        result = evaluate_campaign(results, gate_config())
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["reason"], "insufficient_valid_runs")

    def test_large_regime_shift_fails(self) -> None:
        results = [
            run_result("a0", 0, 1.0),
            run_result("a1", 0, 1.0),
            run_result("b0", 1, 0.4),
            run_result("b1", 1, 0.4),
            run_result("c0", 2, 1.0),
            run_result("c1", 2, 1.0),
        ]
        self.assertEqual(
            evaluate_campaign(results, gate_config())["status"], "FAIL"
        )


if __name__ == "__main__":
    unittest.main()
