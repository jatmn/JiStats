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


def job(name, attempt, duration, *, status="completed", conclusion="success", steps=()):
    now = datetime.now(timezone.utc)
    return {
        "id": attempt * 100 + [DETECT, PORTABLE, CROSS, GATE].index(name),
        "name": name,
        "run_attempt": attempt,
        "status": status,
        "conclusion": conclusion,
        "started_at": (now - timedelta(seconds=duration)).isoformat(),
        "completed_at": now.isoformat() if status == "completed" else None,
        "steps": steps,
    }


class NativeBudgetTest(unittest.TestCase):
    def run_gate(self, *jobs):
        active = job(GATE, 2, 5, status="in_progress", conclusion=None)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "github-output"
            command = [
                sys.executable, str(SCRIPT), "--attempt", "2", "--running-job", str(active["id"]),
                "--github-output", str(output),
            ]
            for dependency in (DETECT, PORTABLE, CROSS):
                command.extend(["--upstream", dependency, "success"])
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


if __name__ == "__main__":
    unittest.main()
