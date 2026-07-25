from __future__ import annotations

import unittest

from pathlib import Path

from uring_slab_tier.fio_device_noise import _direction_metrics, _run_one


class FioDeviceNoiseTest(unittest.TestCase):
    def test_extracts_direction_metrics(self) -> None:
        output = {
            "jobs": [
                {
                    "error": 0,
                    "read": {
                        "io_bytes": 4096,
                        "runtime": 10,
                        "bw_bytes": 409600,
                        "iops": 100,
                        "clat_ns": {
                            "max": 3_000_000,
                            "percentile": {
                                "95.000000": 2_000_000,
                                "99.000000": 2_500_000,
                            },
                        },
                    },
                }
            ]
        }
        metrics = _direction_metrics(output, "read", 4096)
        self.assertTrue(metrics["bytes_match"])
        self.assertEqual(metrics["clat_p95_ms"], 2.0)
        self.assertEqual(metrics["clat_p99_ms"], 2.5)
        self.assertEqual(metrics["clat_max_ms"], 3.0)

    def test_fixed_seed_policy_reuses_same_seed(self) -> None:
        # Exercise the policy branch without invoking fio by replacing the
        # command runner at module scope.
        import uring_slab_tier.fio_device_noise as module

        config = {
            "workload": {
                "seed": 240,
                "seed_policy": "fixed",
                "ioengine": "io_uring",
                "block_bytes": 4096,
                "num_bytes": 4096,
                "iodepth": 1,
            }
        }
        commands = []
        original = module._run_fio

        def fake_run(command):
            commands.append(command)
            direction = (
                "write"
                if any(item == "--rw=randwrite" for item in command)
                else "read"
            )
            empty = {
                "io_bytes": 4096,
                "runtime": 1,
                "bw_bytes": 4096,
                "iops": 1,
                "clat_ns": {
                    "max": 1,
                    "percentile": {"95.000000": 1, "99.000000": 1},
                },
            }
            return {
                "jobs": [
                    {
                        "error": 0,
                        "read": empty if direction == "read" else {},
                        "write": empty if direction == "write" else {},
                    }
                ]
            }

        module._run_fio = fake_run
        try:
            import tempfile

            with tempfile.TemporaryDirectory() as directory:
                result = _run_one(
                    config=config,
                    campaign_id="campaign",
                    run_id="run",
                    window=2,
                    repetition=9,
                    data_file=Path(directory) / "data.bin",
                    run_dir=Path(directory) / "run",
                )
        finally:
            module._run_fio = original
        self.assertEqual(result["status"], "PASS")
        self.assertTrue(all("--randseed=240" in command for command in commands))


if __name__ == "__main__":
    unittest.main()
