from __future__ import annotations

import math
import random
import statistics
from collections.abc import Iterable
from typing import Any


def percentile(values: Iterable[float], q: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    if not 0.0 <= q <= 1.0:
        raise ValueError("q must be in [0, 1]")
    position = (len(ordered) - 1) * q
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    fraction = position - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def _bootstrap_median_ci(
    values: list[float], samples: int, seed: int
) -> tuple[float, float]:
    rng = random.Random(seed)
    medians = []
    for _ in range(samples):
        medians.append(
            statistics.median(rng.choice(values) for _ in range(len(values)))
        )
    return percentile(medians, 0.025), percentile(medians, 0.975)


def describe_metric(
    values: list[float],
    windows: list[int],
    *,
    bootstrap_samples: int,
    bootstrap_seed: int,
) -> dict[str, Any]:
    if not values or len(values) != len(windows):
        raise ValueError("values and windows must be non-empty and equal length")
    median = statistics.median(values)
    deviations = [abs(value - median) for value in values]
    mad = statistics.median(deviations)
    robust_cv = 0.0 if median == 0 and mad == 0 else 1.4826 * mad / abs(median)
    ci_low, ci_high = _bootstrap_median_ci(
        values, bootstrap_samples, bootstrap_seed
    )
    relative_ci_halfwidth = (
        math.inf
        if median == 0
        else (ci_high - ci_low) / (2.0 * abs(median))
    )
    by_window: dict[int, list[float]] = {}
    for window, value in zip(windows, values):
        by_window.setdefault(window, []).append(value)
    window_medians = {
        str(window): statistics.median(window_values)
        for window, window_values in sorted(by_window.items())
    }
    nonzero_window_medians = [
        value for value in window_medians.values() if value > 0
    ]
    window_ratio = (
        max(nonzero_window_medians) / min(nonzero_window_medians)
        if len(nonzero_window_medians) >= 2
        else 1.0
    )
    return {
        "n": len(values),
        "median": median,
        "minimum": min(values),
        "maximum": max(values),
        "mad": mad,
        "robust_cv": robust_cv,
        "bootstrap_median_ci_95": [ci_low, ci_high],
        "relative_ci_halfwidth": relative_ci_halfwidth,
        "window_medians": window_medians,
        "window_max_min_ratio": window_ratio,
    }


def _criterion(value: float, pass_limit: float, fail_limit: float) -> str:
    if value <= pass_limit:
        return "PASS"
    if value > fail_limit:
        return "FAIL"
    return "WARN"


def evaluate_campaign(
    run_results: list[dict[str, Any]], gate: dict[str, Any]
) -> dict[str, Any]:
    valid = [result for result in run_results if result.get("status") == "PASS"]
    correctness_failures = [
        result.get("run_id", "unknown")
        for result in run_results
        if result.get("status") != "PASS"
        or not result.get("correctness", {}).get("pass", False)
    ]
    minimum_valid_runs = int(gate["minimum_valid_runs"])
    if len(valid) < minimum_valid_runs:
        return {
            "status": "FAIL",
            "reason": "insufficient_valid_runs",
            "valid_runs": len(valid),
            "required_valid_runs": minimum_valid_runs,
            "correctness_failures": correctness_failures,
            "metrics": {},
            "criteria": {},
        }
    if correctness_failures:
        return {
            "status": "FAIL",
            "reason": "correctness_failure",
            "valid_runs": len(valid),
            "required_valid_runs": minimum_valid_runs,
            "correctness_failures": correctness_failures,
            "metrics": {},
            "criteria": {},
        }

    metric_specs = {
        "store_throughput_mib_s": "throughput",
        "load_throughput_mib_s": "throughput",
        "store_job_p95_ms": "sojourn_p95",
        "load_job_p95_ms": "sojourn_p95",
    }
    windows = [int(result["window"]) for result in valid]
    bootstrap_samples = int(gate["bootstrap_samples"])
    bootstrap_seed = int(gate["bootstrap_seed"])
    metrics: dict[str, Any] = {}
    criteria: dict[str, str] = {}

    for metric_index, (metric_name, kind) in enumerate(metric_specs.items()):
        values = [float(result["metrics"][metric_name]) for result in valid]
        description = describe_metric(
            values,
            windows,
            bootstrap_samples=bootstrap_samples,
            bootstrap_seed=bootstrap_seed + metric_index,
        )
        metrics[metric_name] = description
        criteria[f"{metric_name}.robust_cv"] = _criterion(
            description["robust_cv"],
            float(gate[f"{kind}_robust_cv_pass"]),
            float(gate[f"{kind}_robust_cv_fail"]),
        )
        criteria[f"{metric_name}.relative_ci_halfwidth"] = _criterion(
            description["relative_ci_halfwidth"],
            float(gate[f"{kind}_relative_ci_halfwidth_pass"]),
            float(gate[f"{kind}_relative_ci_halfwidth_fail"]),
        )
        criteria[f"{metric_name}.window_max_min_ratio"] = _criterion(
            description["window_max_min_ratio"],
            float(gate[f"{kind}_window_ratio_pass"]),
            float(gate[f"{kind}_window_ratio_fail"]),
        )

    statuses = set(criteria.values())
    status = "FAIL" if "FAIL" in statuses else "WARN" if "WARN" in statuses else "PASS"
    return {
        "status": status,
        "reason": "threshold_evaluation",
        "valid_runs": len(valid),
        "required_valid_runs": minimum_valid_runs,
        "correctness_failures": [],
        "metrics": metrics,
        "criteria": criteria,
    }
