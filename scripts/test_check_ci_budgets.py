"""Exercise the budget gate with paginated job histories from reruns."""

import json
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path


SCRIPT = Path(__file__).with_name("check_ci_budgets.py")
DETECT = "Detect build changes"
PORTABLE = "Portable tests with bundled SQLite"
CROSS = "Windows cross-build"
GATE = "CI passed"
SHARDS = tuple(f"Windows tests (shard {index})" for index in range(3))
SUITE_STEP = "Run Windows suite and diagnose intermittent restart stalls"
TIMELINE = datetime(2026, 4, 5, tzinfo=timezone.utc)


def job(name, attempt, duration, *, status="completed", conclusion="success", steps=(), finish=None):
    finish = finish or datetime.now(timezone.utc)
    return {
        "id": attempt * 100 + (DETECT, PORTABLE, CROSS, GATE, *SHARDS).index(name),
        "name": name,
        "run_attempt": attempt,
        "status": status,
        "conclusion": conclusion,
        "started_at": (finish - timedelta(seconds=duration)).isoformat(),
        "completed_at": finish.isoformat() if status == "completed" else None,
        "steps": steps,
    }


def step(name, start, finish):
    return {
        "name": name,
        "status": "completed",
        "conclusion": "success",
        "started_at": (TIMELINE + timedelta(seconds=start)).isoformat(),
        "completed_at": (TIMELINE + timedelta(seconds=finish)).isoformat(),
    }


def shard(index, start, finish, *, attempt=2, duration=None, setup=0):
    return job(
        SHARDS[index], attempt, duration if duration is not None else finish - start + setup,
        steps=[step("Set up job", start - setup, start), step(SUITE_STEP, start, finish)],
        finish=TIMELINE + timedelta(seconds=finish),
    )


class NativeBudgetTest(unittest.TestCase):
    def run_gate(self, *jobs, windows_result=None, shard_names=SHARDS, build_result="success"):
        active = job(GATE, 2, 5, status="in_progress", conclusion=None)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "github-output"
            command = [
                sys.executable, str(SCRIPT), "--attempt", "2", "--running-job", str(active["id"]),
                "--github-output", str(output),
            ]
            for dependency in (DETECT, PORTABLE, CROSS):
                conclusion = "success" if dependency == DETECT else windows_result or build_result
                command.extend(["--upstream", dependency, conclusion])
            if windows_result is not None:
                command.extend(["--windows-result", windows_result])
                for name in shard_names:
                    command.extend(["--windows-shard", name])
            result = subprocess.run(
                command, input=json.dumps([{"jobs": [active]}, {"jobs": jobs}]),
                text=True, capture_output=True, check=False,
            )
            return result, output.read_text(encoding="utf-8") if output.exists() else None

    def test_partial_retry_enforces_the_reused_upstream_budget(self):
        result, output = self.run_gate(job(DETECT, 1, 4), job(PORTABLE, 1, 9), job(CROSS, 1, 45))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Windows cross-build: 45s", result.stdout)
        self.assertEqual(output, "overrun=true\n")

    def test_full_retry_can_recover_after_cancellation_before_measurement(self):
        result, output = self.run_gate(
            job(CROSS, 1, 45),
            job(GATE, 1, 7, conclusion="cancelled"),
            job(CROSS, 2, 15),
            job(PORTABLE, 2, 9),
            job(DETECT, 2, 4),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Windows cross-build: 15s", result.stdout)
        self.assertEqual(output, "overrun=false\n")

    def test_a_recorded_overrun_survives_a_full_retry(self):
        result, output = self.run_gate(
            job(GATE, 1, 32, conclusion="failure", steps=[
                {"name": "Fail on a measured native CI overrun", "conclusion": "failure"},
            ]),
            job(CROSS, 2, 15),
            job(PORTABLE, 2, 9),
            job(DETECT, 2, 4),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Attempt 1 already measured an overrun", result.stdout)
        self.assertEqual(output, "overrun=true\n")

    def test_missing_or_mismatched_upstream_job_fails_closed(self):
        for other_jobs in [[], [job(CROSS, 1, 15, conclusion="skipped")]]:
            with self.subTest(other_jobs=other_jobs):
                result, output = self.run_gate(job(DETECT, 1, 4), job(PORTABLE, 1, 9), *other_jobs)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Rerun all jobs", result.stderr)
                self.assertIsNone(output)

    def run_shard_gate(self, *windows, windows_result="success", shard_names=SHARDS):
        return self.run_gate(
            job(DETECT, 1, 4),
            job(PORTABLE, 1, 9, conclusion=windows_result),
            job(CROSS, 1, 15, conclusion=windows_result),
            *windows,
            windows_result=windows_result,
            shard_names=shard_names,
        )

    def test_windows_wall_time_is_the_envelope_across_shards(self):
        for latest_finish, expected_overrun in ((29, "false"), (30, "true")):
            with self.subTest(latest_finish=latest_finish):
                result, output = self.run_shard_gate(
                    shard(0, 0, 17), shard(1, 5, 25), shard(2, 12, latest_finish),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"Windows suite wall time across shards: {latest_finish}s", result.stdout)
                self.assertEqual(output, f"overrun={expected_overrun}\n")

    def test_windows_shards_still_enforce_job_and_setup_limits(self):
        for slow_shard, report in (
            (shard(0, 0, 10, duration=35), f"{SHARDS[0]}: 35s (limit <35s) **FAIL**"),
            (shard(0, 0, 10, setup=15), "Setup: 15s (limit <15s) **FAIL**"),
        ):
            with self.subTest(report=report):
                result, output = self.run_shard_gate(slow_shard, shard(1, 2, 11), shard(2, 5, 15))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(report, result.stdout)
                self.assertEqual(output, "overrun=true\n")

    def test_current_gate_can_reuse_a_complete_earlier_windows_cohort(self):
        result, output = self.run_shard_gate(
            shard(0, 0, 14, attempt=1), shard(1, 3, 19, attempt=1), shard(2, 6, 21, attempt=1),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Windows suite wall time across shards: 21s", result.stdout)
        self.assertEqual(output, "overrun=false\n")

    def test_mixed_windows_attempts_fail_closed(self):
        result, output = self.run_shard_gate(
            shard(0, 0, 14, attempt=1), shard(1, 3, 19), shard(2, 6, 21, attempt=1),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("different attempts", result.stderr)
        self.assertIn("Rerun all jobs", result.stderr)
        self.assertIsNone(output)

    def test_windows_skips_require_the_expected_conclusion_for_all_shards(self):
        skipped = [job(name, 2, 0, conclusion="skipped") for name in SHARDS]
        result, output = self.run_shard_gate(*skipped, windows_result="skipped")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Windows suite wall time", result.stdout)
        self.assertEqual(output, "overrun=false\n")

        for cohort, conclusion in ((skipped, "success"), ([*skipped[:2], shard(2, 4, 15)], "skipped")):
            with self.subTest(conclusion=conclusion):
                result, output = self.run_shard_gate(*cohort, windows_result=conclusion)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Rerun all jobs", result.stderr)
                self.assertIsNone(output)

    def test_valid_pr_skip_does_not_require_a_nonexpanded_matrix(self):
        result, output = self.run_gate(
            job(DETECT, 2, 4),
            job(PORTABLE, 2, 0, conclusion="skipped"),
            job(CROSS, 2, 0, conclusion="skipped"),
            build_result="skipped",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Windows suite", result.stdout)
        self.assertEqual(output, "overrun=false\n")

    def run_completed_audit(self, *jobs):
        return subprocess.run(
            [sys.executable, str(SCRIPT)], input=json.dumps([{"jobs": jobs}]),
            text=True, capture_output=True, check=False,
        )

    def test_completed_audit_checks_the_full_windows_cohort(self):
        for latest_finish, expected_exit in ((29, 0), (30, 1)):
            with self.subTest(latest_finish=latest_finish):
                result = self.run_completed_audit(
                    shard(0, 0, 17), shard(1, 5, 25), shard(2, 12, latest_finish),
                )
                self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
                self.assertIn(f"Windows suite wall time across shards: {latest_finish}s", result.stdout)
                self.assertNotIn("Windows suite step:", result.stdout)

    def test_completed_audit_times_only_jobs_in_a_partial_retry(self):
        current = shard(2, 25, 42)
        for cohort in ((current,), (shard(0, 0, 17, attempt=1), shard(1, 5, 25, attempt=1), current)):
            with self.subTest(cohort=cohort):
                result = self.run_completed_audit(*cohort)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Windows suite step: 17s", result.stdout)
                self.assertNotIn("Windows suite wall time across shards", result.stdout)

    def test_historical_unsharded_job_reports_its_single_suite_step(self):
        historical = job(GATE, 1, 33, steps=[step(SUITE_STEP, 0, 30)])
        result = self.run_completed_audit(historical)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout.count("Windows suite step: 30s (limit <30s) **FAIL**"), 1)
        self.assertNotIn("Windows suite wall time across shards", result.stdout)

    def test_missing_duplicate_or_incomplete_windows_shard_fails_closed(self):
        ready = [shard(0, 0, 14), shard(1, 3, 19)]
        incomplete = job(SHARDS[2], 2, 0, status="in_progress", conclusion=None)
        failed = job(SHARDS[2], 2, 5, conclusion="failure")
        for cohort in (ready, [*ready, ready[0], shard(2, 6, 20)], [*ready, incomplete], [*ready, failed]):
            with self.subTest(cohort=cohort):
                result, output = self.run_shard_gate(*cohort)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Rerun all jobs", result.stderr)
                self.assertIsNone(output)

    def test_windows_suite_step_must_have_a_unique_successful_timing(self):
        ready = [shard(0, 0, 14), shard(1, 3, 19)]
        successful = step(SUITE_STEP, 1, 2)
        for steps in ([], [successful, successful], [{**successful, "conclusion": "failure"}],
                      [{**successful, "completed_at": None}]):
            with self.subTest(steps=steps):
                result, output = self.run_shard_gate(
                    *ready, job(SHARDS[2], 2, 14, steps=steps),
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Rerun all jobs", result.stderr)
                self.assertIsNone(output)

    def test_duplicate_configured_shard_names_fail_closed(self):
        result, output = self.run_shard_gate(shard(0, 0, 14), shard(1, 3, 19), shard_names=(*SHARDS[:2], SHARDS[1]))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Windows shard names must be distinct", result.stderr)
        self.assertIsNone(output)


if __name__ == "__main__":
    unittest.main()
