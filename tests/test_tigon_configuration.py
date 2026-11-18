import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.args import init_args
from scripts.runner import _run_command_to_csv, get_command, get_data_file_name


class TigonConfigurationTests(unittest.TestCase):
    def parse_args(self, *options):
        argv = [
            "scripts.main",
            "4",
            "5",
            "2",
            "--capture",
            "throughput",
            "--sweep",
            "threads",
            "1",
            "4",
            "1",
            "--plot",
            "none",
            "--data-folder",
            "./tmp-tigon-test-data",
            *options,
        ]
        with patch.object(sys, "argv", argv):
            return init_args()

    def test_ycsb_defaults_are_explicit_in_command_and_artifact_name(self):
        run = self.parse_args("--bench", "tigon")

        with self.assertLogs("scripts.logger", level="INFO") as logs:
            command = get_command(run, "exp_spin", threads=4)
        self.assertIn("--query=rmw", command)
        self.assertIn("--protocol=TwoPL", command)
        self.assertIn("--partition_num=4", command)
        self.assertIn("--read_write_ratio=80", command)
        self.assertIn("--keys=200000", command)
        self.assertIn("Tigon run configuration:", logs.output[0])
        self.assertIn("app=YCSB query=rmw protocol=TwoPL", logs.output[0])
        self.assertIn("servers=127.0.0.1:1234", logs.output[0])

        data_file = get_data_file_name(run, "exp_spin", 0, threads=4)
        self.assertIn("query-rmw", data_file)
        self.assertIn("protocol-TwoPL", data_file)
        self.assertIn("partitions-4", data_file)
        self.assertIn("keys-200000", data_file)

    def test_ycsb_query_mix_and_skew_are_configurable(self):
        run = self.parse_args(
            "--bench",
            "tigon_ycsb",
            "--tigon-query",
            "scan",
            "--tigon-zipf",
            "0.8",
        )

        command = get_command(run, "mcs", threads=4)
        self.assertIn("--query=scan", command)
        self.assertIn("--zipf=0.8", command)
        self.assertIn("query-scan", get_data_file_name(run, "mcs", 0, threads=4))

    def test_partition_ratio_scales_with_each_swept_thread_count(self):
        run = self.parse_args(
            "--bench",
            "tigon_ycsb",
            "--tigon-partition-ratio",
            "half",
        )

        self.assertIn("--partition_num=2", get_command(run, "mcs", threads=4))
        self.assertIn("--partition_num=1", get_command(run, "mcs", threads=1))
        self.assertIn("partition-ratio-half", get_data_file_name(run, "mcs", 0, threads=4))

    def test_tpc_c_workload_and_partition_count_are_configurable(self):
        run = self.parse_args(
            "--bench",
            "tigon_tpcc",
            "--tigon-query",
            "neworder",
            "--tigon-protocol",
            "Silo",
            "--tigon-partitions",
            "2",
            "--tigon-warmup-seconds",
            "2",
            "--tigon-neworder-dist",
            "25",
            "--tigon-payment-dist",
            "35",
        )

        command = get_command(run, "ticket", threads=4)
        self.assertIn("--query=neworder", command)
        self.assertIn("--protocol=Silo", command)
        self.assertIn("--partition_num=2", command)
        self.assertIn("--time_to_warmup=2", command)
        self.assertIn("--neworder_dist=25", command)
        self.assertIn("--payment_dist=35", command)
        self.assertNotIn("--keys=200000", command)

        data_file = get_data_file_name(run, "ticket", 0, threads=4)
        self.assertIn("query-neworder", data_file)
        self.assertIn("neworder-25", data_file)
        self.assertIn("payment-35", data_file)

    def test_rejects_query_for_wrong_tigon_application(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parse_args("--bench", "tigon_tpcc", "--tigon-query", "rmw")

    def test_default_reuse_can_read_legacy_tigon_csv_names(self):
        with tempfile.TemporaryDirectory() as data_folder:
            run = self.parse_args(
                "--bench",
                "tigon_ycsb",
                "--reuse-data",
                "--data-folder",
                data_folder,
            )
            legacy_path = Path(data_folder) / (
                "exp_spin-tigon_ycsb-throughput-rep0-threads=4.csv"
            )
            legacy_path.write_text("0,5.0,1\n", encoding="utf-8")

            self.assertEqual(
                get_data_file_name(run, "exp_spin", 0, threads=4),
                str(legacy_path),
            )

    def test_child_failure_reports_signal_and_both_output_streams(self):
        result = subprocess.CompletedProcess(
            args=["benchmark"],
            returncode=-9,
            stdout=b"benchmark stdout\n",
            stderr=b"last diagnostic\n",
        )
        run = self.parse_args()  # placement none: no NUMA environment for the child
        with patch("scripts.runner.subprocess.run", return_value=result):
            with self.assertRaises(RuntimeError) as error:
                _run_command_to_csv(run, ["benchmark"], timeout_s=10)

        message = str(error.exception)
        self.assertIn("terminated by SIGKILL (9)", message)
        self.assertIn("benchmark stdout", message)
        self.assertIn("last diagnostic", message)


if __name__ == "__main__":
    unittest.main()
