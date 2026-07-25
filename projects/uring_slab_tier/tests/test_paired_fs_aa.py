from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path

from uring_slab_tier.paired_fs_aa import (
    _load_config,
    _make_schedule,
    _schedule_sha256,
)


PROJECT_ROOT = Path(__file__).resolve().parents[1]


class PairedFsAaConfigTest(unittest.TestCase):
    def test_formal_config_has_fixed_twelve_gib_per_arm(self) -> None:
        config = _load_config(
            PROJECT_ROOT / "configs/native_fs_paired_aa_v1.json"
        )
        workload = config["workload"]
        measured_bytes = (
            int(workload["block_bytes"])
            * int(workload["dataset_blocks"])
            * int(workload["measurement_passes"])
        )
        self.assertEqual(measured_bytes, 12 * 1024**3)
        self.assertEqual(config["gate"]["required_complete_pairs"], 30)

    def test_store_and_load_orders_are_balanced_per_window(self) -> None:
        config = _load_config(
            PROJECT_ROOT / "configs/native_fs_paired_aa_v1.json"
        )
        schedule = _make_schedule(config)
        self.assertEqual(len(schedule), 30)
        self.assertEqual(_schedule_sha256(schedule), _schedule_sha256(schedule))
        for window in range(3):
            items = [item for item in schedule if item["window"] == window]
            for field in ("store_order", "load_order"):
                orders = [item[field] for item in items]
                self.assertEqual(orders.count("AB"), 5)
                self.assertEqual(orders.count("BA"), 5)

    def test_drop_caches_and_unknown_fields_are_rejected(self) -> None:
        source = json.loads(
            (
                PROJECT_ROOT / "configs/native_fs_paired_aa_smoke.json"
            ).read_text()
        )
        for mutation, message in (
            (("drop_caches", True), "drop_caches"),
            (("unknown", 1), "unknown top-level"),
        ):
            config = copy.deepcopy(source)
            key, value = mutation
            if key == "drop_caches":
                config["safety"][key] = value
            else:
                config[key] = value
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "config.json"
                path.write_text(json.dumps(config))
                with self.assertRaisesRegex(ValueError, message):
                    _load_config(path)


if __name__ == "__main__":
    unittest.main()
