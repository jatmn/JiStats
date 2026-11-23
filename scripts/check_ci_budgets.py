"""Evaluate GitHub job timing after all tests and runner teardown finish."""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


OVERRUN_STEP = "Fail on a measured native CI overrun"
WINDOWS_SUITE_STEP = "Run Windows suite and diagnose intermittent restart stalls"
WINDOWS_SHARD_NAMES = tuple(f"Windows tests (shard {index})" for index in range(3))


def timing(item):
    if not item.get("started_at") or not item.get("completed_at"):
        raise ValueError(f'Incomplete timing for {item["name"]}.')
    start = datetime.fromisoformat(item["started_at"].replace("Z", "+00:00"))
    finish = datetime.fromisoformat(item["completed_at"].replace("Z", "+00:00"))
    if finish < start:
        raise ValueError(f'Invalid timing for {item["name"]}.')
    return start, finish


def elapsed(item):
    try:
        start, finish = timing(item)
    except ValueError as error:
        sys.exit(f"{error} Rerun all jobs if the timings are unavailable.")
    return (finish - start).total_seconds()


def check(name, duration, limit):
    over = duration >= limit
    print(f"- {name}: {duration:g}s (limit <{limit}s)" + (" **FAIL**" if over else ""))
    return over


def latest_completed_job(jobs, name, attempt, conclusion):
    candidates = [job for job in jobs if job["name"] == name and job["run_attempt"] <= attempt]
    latest_attempt = max((job["run_attempt"] for job in candidates), default=None)
    latest = [job for job in candidates if job["run_attempt"] == latest_attempt]
    if len(latest) != 1 or latest[0]["status"] != "completed" or latest[0]["conclusion"] != conclusion:
        raise ValueError(f"Could not identify the upstream execution for {name}.")
    return latest[0]


def native_jobs(jobs, current_id, attempt, upstream):
    """Find the active gate and the executions whose results it depends on."""
    current = next((job for job in jobs if job["id"] == current_id), None)
    if current is None or current["run_attempt"] != attempt or current["status"] != "in_progress":
        raise ValueError("The active CI gate is missing from this attempt.")

    selected = [latest_completed_job(jobs, name, attempt, conclusion) for name, conclusion in upstream]

    for job in jobs:
        if job["name"] == current["name"] and job["run_attempt"] < attempt:
            if any(step["name"] == OVERRUN_STEP and step["conclusion"] == "failure"
                   for step in job["steps"]):
                print(f'- Attempt {job["run_attempt"]} already measured an overrun.')
                return [*selected, current], True
    return [*selected, current], False


def windows_shard_cohort(jobs, shard_names, attempt, conclusion):
    if len(shard_names) != len(set(shard_names)):
        raise ValueError("Windows shard names must be distinct.")
    shards = [latest_completed_job(jobs, name, attempt, conclusion) for name in shard_names]
    if conclusion == "skipped":
        return shards, None
    if len({job["run_attempt"] for job in shards}) != 1:
        raise ValueError("Windows shards came from different attempts and cannot be timed together.")

    return shards, windows_suite_span(shards, require_complete=True)


def windows_suite_span(shards, *, require_complete):
    spans = []
    for job in shards:
        steps = [step for step in job["steps"] if step["name"] == WINDOWS_SUITE_STEP]
        if len(steps) == 1 and steps[0]["status"] == "completed" and steps[0]["conclusion"] == "success":
            spans.append(timing(steps[0]))
        elif require_complete or len(steps) > 1:
            raise ValueError(f'Could not identify the completed Windows suite step for {job["name"]}.')
        else:
            return None
    start = min(start for start, _ in spans)
    finish = max(finish for _, finish in spans)
    return (finish - start).total_seconds()


def completed_windows_span(jobs):
    """Measure a successful suite even if a later step failed."""
    shards = [job for job in jobs if job["name"] in WINDOWS_SHARD_NAMES and job["status"] == "completed"]
    if {job["name"] for job in shards} != set(WINDOWS_SHARD_NAMES):
        return None
    if len({job["run_attempt"] for job in shards}) != 1:
        return None
    if len(shards) != len(WINDOWS_SHARD_NAMES):
        raise ValueError("Could not identify unique Windows shard jobs for this attempt.")
    return windows_suite_span(shards, require_complete=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--running-job", type=int, help="also check elapsed time of this active job")
    parser.add_argument("--attempt", type=int)
    parser.add_argument("--upstream", action="append", nargs=2, metavar=("JOB", "CONCLUSION"))
    parser.add_argument("--windows-shard", action="append", default=[], metavar="JOB")
    parser.add_argument("--windows-result", choices=("success", "skipped"))
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    if bool(args.windows_shard) != bool(args.windows_result) or (
        args.windows_shard and args.running_job is None
    ):
        parser.error("--windows-shard and --windows-result require each other and --running-job")
    setup_prefixes = ("set up job", "run actions/checkout", "check out the tested",
                      "checkout trusted", "install ", "run actions/download-artifact",
                      "initialize codeql", "fetch c++ dependency", "restore compiler results")
    jobs = [job for page in json.load(sys.stdin) for job in page["jobs"]]
    if not jobs:
        sys.exit("No jobs returned for this attempt")
    failed = False
    windows_span = None
    if args.running_job is not None:
        if args.attempt is None or not args.upstream or args.github_output is None:
            parser.error("--running-job requires --attempt, --upstream and --github-output")
        try:
            selected, failed = native_jobs(jobs, args.running_job, args.attempt, args.upstream)
            if args.windows_shard:
                shards, windows_span = windows_shard_cohort(
                    jobs, args.windows_shard, args.attempt, args.windows_result,
                )
                selected = [*selected[:-1], *shards, selected[-1]]
            jobs = selected
        except ValueError as error:
            sys.exit(f"{error} Rerun all jobs if the upstream timings are unavailable.")
    else:
        try:
            windows_span = completed_windows_span(jobs)
        except ValueError as error:
            sys.exit(str(error))

    if windows_span is not None:
        failed |= check("Windows suite wall time across shards", windows_span, 30)
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
            if windows_span is None and step["name"] == WINDOWS_SUITE_STEP:
                failed |= check("Windows suite step", elapsed(step), 30)
    if args.running_job is not None:
        with args.github_output.open("a", encoding="utf-8") as output:
            print(f"overrun={str(failed).lower()}", file=output)
        return 0
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
