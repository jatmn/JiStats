# Evaluate completed GitHub job pages supplied with jq --slurp.
def elapsed:
    (.completed_at | fromdateiso8601) - (.started_at | fromdateiso8601);

def timing($name; $duration; $limit):
    if $report_only then
        {line: "- \($name): \($duration)s (report only)"}
    else
        ($duration >= $limit) as $failed
        | {
            line: ("- \($name): \($duration)s (limit <\($limit)s)"
                + (if $failed then " **FAIL**" else "" end)),
            failed: $failed
        }
    end;

def setup_step:
    (.name | ascii_downcase) as $name
    | any(
        "set up job", "run actions/checkout", "check out the tested",
        "checkout trusted", "install ", "run actions/download-artifact",
        "initialize codeql", "fetch c++ dependency", "restore compiler results";
        . as $prefix | $name | startswith($prefix)
    );

def windows_suite:
    select(.name == "Run Windows suite and diagnose intermittent restart stalls");

def job_checks:
    if .conclusion == "skipped" then
        {line: "- \(.name): skipped in source workflow"}
    elif .status != "completed" then
        "Job \(.id) is still running\n" | halt_error(1)
    else
        (if .conclusion != "success" then
            {line: "- \(.name): source conclusion \(.conclusion)"}
        else empty end),
        timing(.name; elapsed; 35),
        ([.steps[] | select(.conclusion != "cancelled" and .conclusion != "skipped")] as $steps
        | timing("Setup"; ([$steps[] | select(setup_step) | elapsed] | add // 0); 15),
          ($steps[] | windows_suite
           | timing("Windows suite step"; elapsed; 30)))
    end;

def windows_shard_check:
    [.[] | .jobs[]
     | select(.name | test("^Windows tests \\(shard [0-9]+\\)$"))
     | .steps[]? | windows_suite | select(.conclusion == "success")] as $suites
    | if ($suites | length) > 1 then
        ([$suites[].completed_at | fromdateiso8601] | max) as $last
        | ([$suites[].started_at | fromdateiso8601] | min) as $first
        | timing("Windows suite span across shards in this attempt"; $last - $first; 30)
      else empty end;

# Emit each check before evaluating the next one. The final null marks the end
# of the check stream and lets the audit return its aggregate budget status.
foreach ((.[] | .jobs[] | job_checks), windows_shard_check, null) as $check (
    {seen: false, failed: false};
    {seen: (.seen or ($check != null)), failed: (.failed or $check.failed)};
    if $check != null then $check.line
    elif .seen == false then "No jobs returned for this attempt\n" | halt_error(1)
    elif .failed then "" | halt_error(1)
    else empty end
)
