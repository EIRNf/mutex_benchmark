import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
START_SCRIPT = ROOT / "start.sh"


class StartScriptTests(unittest.TestCase):
    def run_start(self, *arguments, output_root=None):
        env = os.environ.copy()
        env.update(
            {
                "RUN_TIME": "10",
                "RUNS": "1",
                "THREAD_START": "1",
                "MAX_THREADS": "1",
                "THREAD_INTERVAL": "1",
                "DRAM_NODE": "0",
                "CXL_NODE": "1",
                "RUN_NATIVE": "1",
                "RUN_TIGON": "1",
                "RUN_DRAM": "1",
                "RUN_CXL_UNMODIFIED": "0",
                "RUN_CXL_LOW": "0",
                "RUN_CXL_HIGH": "0",
                "TIGON_WARMUP": "1",
                "TIGON_PARTITION_MODES": "threads",
                "LOCK_SET": "network",
                "TIGON_LOCKS": "exp_spin",
                "CXL_LATENCY_PCI": "",
            }
        )
        if output_root is not None:
            env["OUTPUT_ROOT"] = str(output_root)
        return subprocess.run(
            ["bash", str(START_SCRIPT), *arguments],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_lock_set_flag_applies_to_native_and_tigon_preview(self):
        locks = "spin,exp_spin,cxl_fast_exp,ticket,mcs_local"
        with tempfile.TemporaryDirectory() as temp_dir:
            output_root = Path(temp_dir) / "output"
            result = self.run_start(
                "--list-runs",
                "--lock-set",
                "hardware_cxl",
                output_root=output_root,
            )
            self.assertFalse(output_root.exists())

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count(f"locks={{{locks}}}"), 4)
        self.assertIn("lock_set=hardware_cxl", result.stdout)
        self.assertIn("Benchmark executions: 20", result.stdout)
        self.assertIn("Estimated timed workload: 210 seconds", result.stdout)

    def test_unknown_lock_set_is_rejected(self):
        result = self.run_start("--list-runs", "--lock-set", "not_a_real_set")

        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown lock set 'not_a_real_set'", result.stderr)


if __name__ == "__main__":
    unittest.main()
