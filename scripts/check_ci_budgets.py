"""Evaluate GitHub job timing after all tests and runner teardown finish."""

import json
import sys
from datetime import datetime


def elapsed(item):
    return (datetime.fromisoformat(item["completed_at"].replace("Z", "+00:00"))
            - datetime.fromisoformat(item["started_at"].replace("Z", "+00:00"))).total_seconds()


def check(name, duration, limit):
    over = duration >= limit
    print(f"- {name}: {duration:g}s (limit <{limit}s)" + (" **FAIL**" if over else ""))
    return over


def main():
    setup_prefixes = ("set up job", "run actions/checkout", "check out the tested",
                      "checkout trusted", "install ", "run actions/download-artifact",
                      "initialize codeql", "fetch c++ dependency", "restore compiler results")
    jobs = [job for page in json.load(sys.stdin) for job in page["jobs"]]
    if not jobs:
        sys.exit("No jobs returned for this attempt")
    failed = False
    for job in jobs:
        if job["conclusion"] == "skipped":
            print(f'- {job["name"]}: skipped in source workflow')
            continue
        if job["status"] != "completed":
            sys.exit(f'Job {job["id"]} is still running')
        if job["conclusion"] != "success":
            print(f'- {job["name"]}: source conclusion {job["conclusion"]}')
            failed = True
        failed |= check(job["name"], elapsed(job), 35)
        steps = [s for s in job["steps"] if s["conclusion"] not in ("cancelled", "skipped")]
        setup = sum(elapsed(s) for s in steps if s["name"].lower().startswith(setup_prefixes))
        failed |= check("Setup", setup, 15)
        for step in steps:
            if step["name"] == "Run Windows suite and diagnose intermittent restart stalls":
                failed |= check("Windows suite step", elapsed(step), 30)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
