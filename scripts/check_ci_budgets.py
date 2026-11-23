"""Evaluate GitHub job timing after all tests and runner teardown finish."""

import argparse
import json
import sys
from datetime import datetime, timezone


def elapsed(item):
    return (datetime.fromisoformat(item["completed_at"].replace("Z", "+00:00"))
            - datetime.fromisoformat(item["started_at"].replace("Z", "+00:00"))).total_seconds()


def check(name, duration, limit):
    over = duration >= limit
    print(f"- {name}: {duration:g}s (limit <{limit}s)" + (" **FAIL**" if over else ""))
    return over


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--running-job", type=int, help="also check elapsed time of this active job")
    current = parser.parse_args().running_job
    setup_prefixes = ("set up job", "run actions/checkout", "check out the tested",
                      "checkout trusted", "install ", "run actions/download-artifact",
                      "initialize codeql", "fetch c++ dependency", "restore compiler results")
    jobs = [job for page in json.load(sys.stdin) for job in page["jobs"]]
    if not jobs:
        sys.exit("No jobs returned for this attempt")
    if current is not None and sum(j["id"] == current for j in jobs) != 1:
        sys.exit("The running job is missing from this attempt")
    failed = False
    for job in jobs:
        if job["conclusion"] == "skipped":
            print(f'- {job["name"]}: skipped in source workflow')
            continue
        if job["id"] == current and job["status"] == "in_progress":
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
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
