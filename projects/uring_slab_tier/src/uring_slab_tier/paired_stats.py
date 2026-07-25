from __future__ import annotations

import itertools
import math
import random
import statistics
from collections import defaultdict
from collections.abc import Iterable
from typing import Any

from uring_slab_tier.noise_gate import percentile


def balanced_pair_orders(
    *,
    windows: int,
    pairs_per_window: int,
    seed: int,
) -> list[dict[str, int | str]]:
    """Build a deterministic, window-balanced AB/BA execution sequence."""
    if windows <= 0 or pairs_per_window <= 0:
        raise ValueError("windows and pairs_per_window must be positive")
    if pairs_per_window % 2:
        raise ValueError("pairs_per_window must be even for exact AB/BA balance")

    rng = random.Random(seed)
    sequence: list[dict[str, int | str]] = []
    for window in range(windows):
        orders = ["AB"] * (pairs_per_window // 2)
        orders.extend(["BA"] * (pairs_per_window // 2))
        rng.shuffle(orders)
        for pair_index, order in enumerate(orders):
            sequence.append(
                {
                    "window": window,
                    "pair_index": pair_index,
                    "order": order,
                }
            )
    return sequence


def _pearson(values_a: list[float], values_b: list[float]) -> float | None:
    if len(values_a) != len(values_b) or len(values_a) < 2:
        return None
    mean_a = statistics.mean(values_a)
    mean_b = statistics.mean(values_b)
    centered_a = [value - mean_a for value in values_a]
    centered_b = [value - mean_b for value in values_b]
    denominator = math.sqrt(
        sum(value * value for value in centered_a)
        * sum(value * value for value in centered_b)
    )
    if denominator == 0:
        return None
    return (
        sum(a * b for a, b in zip(centered_a, centered_b)) / denominator
    )


def _robust_cv(values: list[float]) -> float:
    median = statistics.median(values)
    mad = statistics.median(abs(value - median) for value in values)
    if median == 0:
        return 0.0 if mad == 0 else math.inf
    return 1.4826 * mad / abs(median)


def _bounded_status(value: float, pass_limit: float, fail_limit: float) -> str:
    if value <= pass_limit:
        return "PASS"
    if value <= fail_limit:
        return "CONDITIONAL"
    return "FAIL"


def _stratified_bootstrap_median_ci(
    effects_by_window: dict[int, list[float]],
    *,
    samples: int,
    seed: int,
) -> tuple[float, float]:
    if samples <= 0:
        raise ValueError("bootstrap samples must be positive")
    rng = random.Random(seed)
    medians: list[float] = []
    ordered_groups = [
        effects_by_window[window] for window in sorted(effects_by_window)
    ]
    for _ in range(samples):
        resampled: list[float] = []
        for group in ordered_groups:
            resampled.extend(rng.choice(group) for _ in range(len(group)))
        medians.append(statistics.median(resampled))
    return percentile(medians, 0.025), percentile(medians, 0.975)


def _sign_flip_medians(
    effects: list[float],
    *,
    samples: int,
    seed: int,
) -> list[float]:
    if samples <= 0:
        raise ValueError("sign-flip samples must be positive")
    exact_assignments = 1 << len(effects)
    if exact_assignments <= samples:
        return [
            statistics.median(
                sign * effect for sign, effect in zip(signs, effects)
            )
            for signs in itertools.product((-1.0, 1.0), repeat=len(effects))
        ]

    rng = random.Random(seed)
    return [
        statistics.median(
            effect if rng.getrandbits(1) else -effect for effect in effects
        )
        for _ in range(samples)
    ]


def _detection_power(
    effects_by_window: dict[int, list[float]],
    *,
    minimum_effect_log: float,
    critical_log: float,
    samples: int,
    seed: int,
) -> float:
    """Empirical power for detecting a positive paired-median shift.

    The observed A/A magnitudes define the noise distribution. Each simulated
    campaign keeps the registered window sizes, resamples magnitudes within a
    window, randomizes their signs, then adds the target log effect.
    """
    if samples <= 0:
        raise ValueError("power samples must be positive")
    rng = random.Random(seed)
    magnitudes_by_window = {
        window: [abs(effect) for effect in effects]
        for window, effects in effects_by_window.items()
    }
    detections = 0
    for _ in range(samples):
        simulated: list[float] = []
        for window in sorted(magnitudes_by_window):
            magnitudes = magnitudes_by_window[window]
            for _ in range(len(magnitudes)):
                noise = rng.choice(magnitudes)
                if not rng.getrandbits(1):
                    noise = -noise
                simulated.append(noise + minimum_effect_log)
        if statistics.median(simulated) > critical_log:
            detections += 1
    return detections / samples


def _metric_effect(
    run_a: dict[str, Any],
    run_b: dict[str, Any],
    *,
    metric_name: str,
    higher_is_better: bool,
) -> tuple[float, float, float]:
    value_a = float(run_a["metrics"][metric_name])
    value_b = float(run_b["metrics"][metric_name])
    if value_a <= 0 or value_b <= 0:
        raise ValueError(f"{metric_name} values must be positive")
    raw_log_ratio = math.log(value_b / value_a)
    effect = raw_log_ratio if higher_is_better else -raw_log_ratio
    return value_a, value_b, effect


def evaluate_aa_campaign(
    pair_results: list[dict[str, Any]],
    gate: dict[str, Any],
) -> dict[str, Any]:
    """Evaluate a fixed-size FS/FS paired A/A qualification campaign."""
    required_pairs = int(gate["required_complete_pairs"])
    complete_pairs = [
        pair
        for pair in pair_results
        if pair.get("status") == "PASS"
        and pair.get("run_a", {}).get("status") == "PASS"
        and pair.get("run_b", {}).get("status") == "PASS"
    ]
    failed_pair_ids = [
        str(pair.get("pair_id", "unknown"))
        for pair in pair_results
        if pair not in complete_pairs
    ]
    if len(complete_pairs) != required_pairs:
        return {
            "status": "FAIL",
            "reason": "incomplete_fixed_campaign",
            "complete_pairs": len(complete_pairs),
            "required_complete_pairs": required_pairs,
            "failed_pair_ids": failed_pair_ids,
            "metrics": {},
            "criteria": {},
        }

    metric_name = str(gate["primary_metric"])
    higher_is_better = bool(gate["higher_is_better"])
    effects: list[float] = []
    effects_by_window: dict[int, list[float]] = defaultdict(list)
    position_effects: list[float] = []
    values_a: list[float] = []
    values_b: list[float] = []
    for pair in complete_pairs:
        value_a, value_b, effect = _metric_effect(
            pair["run_a"],
            pair["run_b"],
            metric_name=metric_name,
            higher_is_better=higher_is_better,
        )
        values_a.append(value_a)
        values_b.append(value_b)
        effects.append(effect)
        effects_by_window[int(pair["window"])].append(effect)
        load_order = str(pair.get("load_order", ""))
        if load_order == "AB":
            position_effects.append(effect)
        elif load_order == "BA":
            position_effects.append(-effect)
        else:
            raise ValueError(
                f"pair {pair.get('pair_id', 'unknown')} lacks AB/BA load_order"
            )

    bootstrap_ci = _stratified_bootstrap_median_ci(
        dict(effects_by_window),
        samples=int(gate["bootstrap_samples"]),
        seed=int(gate["bootstrap_seed"]),
    )
    sign_flip_medians = _sign_flip_medians(
        effects,
        samples=int(gate["sign_flip_samples"]),
        seed=int(gate["sign_flip_seed"]),
    )
    noise_floor_log = percentile(
        (abs(value) for value in sign_flip_medians), 0.95
    )
    observed_median_log = statistics.median(effects)
    minimum_effect_log = math.log1p(float(gate["minimum_detectable_effect"]))
    detection_power = _detection_power(
        dict(effects_by_window),
        minimum_effect_log=minimum_effect_log,
        critical_log=noise_floor_log,
        samples=int(gate["power_samples"]),
        seed=int(gate["power_seed"]),
    )

    window_medians = {
        str(window): math.expm1(statistics.median(window_effects))
        for window, window_effects in sorted(effects_by_window.items())
    }
    maximum_absolute_window_effect = max(
        abs(value) for value in window_medians.values()
    )
    absolute_effects = [abs(effect) for effect in effects]
    effect_median = statistics.median(effects)
    effect_mad = statistics.median(
        abs(effect - effect_median) for effect in effects
    )
    position_median_log = statistics.median(position_effects)
    metrics = {
        "primary_metric": metric_name,
        "n_pairs": len(effects),
        "observed_paired_median_effect": math.expm1(observed_median_log),
        "stratified_bootstrap_median_ci_95": [
            math.expm1(bootstrap_ci[0]),
            math.expm1(bootstrap_ci[1]),
        ],
        "sign_flip_paired_median_noise_floor_95": math.expm1(
            noise_floor_log
        ),
        "minimum_detectable_effect": math.expm1(minimum_effect_log),
        "empirical_detection_power": detection_power,
        "absolute_pair_effect_median": math.expm1(
            statistics.median(absolute_effects)
        ),
        "absolute_pair_effect_p75": math.expm1(
            percentile(absolute_effects, 0.75)
        ),
        "absolute_pair_effect_p90": math.expm1(
            percentile(absolute_effects, 0.90)
        ),
        "paired_effect_mad": math.expm1(effect_mad),
        "paired_value_pearson": _pearson(values_a, values_b),
        "arm_a_unpaired_robust_cv": _robust_cv(values_a),
        "arm_b_unpaired_robust_cv": _robust_cv(values_b),
        "window_paired_median_effects": window_medians,
        "maximum_absolute_window_effect": maximum_absolute_window_effect,
        "position_median_effect": math.expm1(position_median_log),
    }

    pass_bias = float(gate["pass_absolute_aa_bias"])
    max_bias = float(gate["max_absolute_aa_bias"])
    pass_noise_floor = float(gate["pass_noise_floor"])
    max_noise_floor = float(gate["max_noise_floor"])
    minimum_power = float(gate["minimum_power"])
    pass_window_effect = float(gate["pass_absolute_window_effect"])
    max_window_effect = float(gate["max_absolute_window_effect"])
    pass_position_effect = float(gate["pass_absolute_position_effect"])
    max_position_effect = float(gate["max_absolute_position_effect"])
    criteria = {
        "aa_ci_contains_zero": (
            "PASS" if bootstrap_ci[0] <= 0.0 <= bootstrap_ci[1] else "FAIL"
        ),
        "aa_absolute_bias": _bounded_status(
            abs(math.expm1(observed_median_log)),
            pass_bias,
            max_bias,
        ),
        "paired_median_noise_floor": _bounded_status(
            math.expm1(noise_floor_log),
            pass_noise_floor,
            max_noise_floor,
        ),
        "minimum_effect_detection_power": (
            "PASS" if detection_power >= minimum_power else "FAIL"
        ),
        "maximum_absolute_window_effect": _bounded_status(
            maximum_absolute_window_effect,
            pass_window_effect,
            max_window_effect,
        ),
        "absolute_position_effect": _bounded_status(
            abs(math.expm1(position_median_log)),
            pass_position_effect,
            max_position_effect,
        ),
    }
    statuses = set(criteria.values())
    status = (
        "FAIL"
        if "FAIL" in statuses
        else "CONDITIONAL"
        if "CONDITIONAL" in statuses
        else "PASS"
    )
    return {
        "status": status,
        "reason": "paired_aa_qualification",
        "complete_pairs": len(complete_pairs),
        "required_complete_pairs": required_pairs,
        "failed_pair_ids": [],
        "metrics": metrics,
        "criteria": criteria,
    }
