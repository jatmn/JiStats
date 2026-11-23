"""Evaluate GitHub job timing after all tests and runner teardown finish."""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


OVERRUN_STEP = "Fail on a measured native CI overrun"


def elapsed(item):
    if not item.get("started_at") or not item.get("completed_at"):
        sys.exit(f'Incomplete timing for {item["name"]}. Rerun all jobs if the timings are unavailable.')
    return (datetime.fromisoformat(item["completed_at"].replace("Z", "+00:00"))
            - datetime.fromisoformat(item["started_at"].replace("Z", "+00:00"))).total_seconds()


def check(name, duration, limit):
    over = duration >= limit
    print(f"- {name}: {duration:g}s (limit <{limit}s)" + (" **FAIL**" if over else ""))
    return over


def native_jobs(jobs, current_id, attempt, upstream):
    """Find the active gate and the executions whose results it depends on."""
    current = next((job for job in jobs if job["id"] == current_id), None)
    if current is None or current["run_attempt"] != attempt or current["status"] != "in_progress":
        raise ValueError("The active CI gate is missing from this attempt.")

    selected = []
    for name, conclusion in upstream:
        candidates = (job for job in jobs if job["name"] == name and job["run_attempt"] <= attempt)
        job = max(candidates, key=lambda item: item["run_attempt"], default=None)
        if job is None or job["status"] != "completed" or job["conclusion"] != conclusion:
            raise ValueError(f"Could not identify the upstream execution for {name}.")
        selected.append(job)

    for job in jobs:
        if job["name"] == current["name"] and job["run_attempt"] < attempt:
            if any(step["name"] == OVERRUN_STEP and step["conclusion"] == "failure"
                   for step in job["steps"]):
                print(f'- Attempt {job["run_attempt"]} already measured an overrun.')
                return [*selected, current], True
    return [*selected, current], False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--running-job", type=int, help="also check elapsed time of this active job")
    parser.add_argument("--attempt", type=int)
    parser.add_argument("--upstream", action="append", nargs=2, metavar=("JOB", "CONCLUSION"))
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    setup_prefixes = ("set up job", "run actions/checkout", "check out the tested",
                      "checkout trusted", "install ", "run actions/download-artifact",
                      "initialize codeql", "fetch c++ dependency", "restore compiler results")
    jobs = [job for page in json.load(sys.stdin) for job in page["jobs"]]
    if not jobs:
        sys.exit("No jobs returned for this attempt")
    failed = False
    if args.running_job is not None:
        if args.attempt is None or not args.upstream or args.github_output is None:
            parser.error("--running-job requires --attempt, --upstream and --github-output")
        try:
            jobs, failed = native_jobs(jobs, args.running_job, args.attempt, args.upstream)
        except ValueError as error:
            sys.exit(f"{error} Rerun all jobs if the upstream timings are unavailable.")

    for job in jobs:
        if job["conclusion"] == "skipped":
            print(f'- {job["name"]}: skipped in source workflow')
            continue
        if job["id"] == args.running_job:
            # GitHub records the final teardown timestamp only after this check.
            job = {**job, "completed_at": datetime.now(timezone.utc).isoformat()}
        elif job["status"] != "completed":
            sys.exit(f'Job {job["id"]} is still running')
        if job["conclusion"] not in (None, "success"):
            print(f'- {job["name"]}: source conclusion {job["conclusion"]}')
        failed |= check(job["name"], elapsed(job), 35)
        steps = [s for s in job["steps"] if s["status"] == "completed"
                 and s["conclusion"] not in ("cancelled", "skipped")]
        setup = sum(elapsed(s) for s in steps if s["name"].lower().startswith(setup_prefixes))
        failed |= check("Setup", setup, 15)
        for step in steps:
            if step["name"] == "Run Windows suite and diagnose intermittent restart stalls":
                failed |= check("Windows suite step", elapsed(step), 30)
    if args.running_job is not None:
        with args.github_output.open("a", encoding="utf-8") as output:
            print(f"overrun={str(failed).lower()}", file=output)
        return 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
