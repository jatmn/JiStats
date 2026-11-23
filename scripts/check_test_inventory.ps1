param(
    [Parameter(Mandatory)][string] $Inventory,
    [Parameter(Mandatory)][string] $Results,
    [Parameter(Mandatory)][string] $Output,
    [Parameter(Mandatory)][string[]] $ExpectedFiles
)
$ErrorActionPreference = 'Stop'
[xml]$registered = Get-Content -Raw -LiteralPath $Inventory
[xml]$executed = Get-Content -Raw -LiteralPath $Results

function Get-SourcePath([string] $path) {
    if ($path.Replace('\', '/') -match '(?:^|/)((?:tests|src)/.+)$') {
        return $Matches[1]
    }
    throw "Unexpected test source path: $path"
}

$pending = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($case in $registered.MatchingTests.TestCase) {
    $file = Get-SourcePath $case.SourceInfo.File
    if (!$pending.Add("$file`n$($case.Name.Trim())")) {
        throw "Duplicate registered case in $file at $($case.SourceInfo.Line)"
    }
}
if ($pending.Count -eq 0) { throw 'The executable registered no tests.' }

$timings = @{}
foreach ($case in $executed.Catch2TestRun.TestCase) {
    $file = Get-SourcePath $case.filename
    if (!$pending.Remove("$file`n$($case.name)")) { throw "Unexpected or repeated test: $($case.name)" }
    $result = $case.OverallResult
    if ($result.success -cne 'true' -or $result.skips -cne '0' -or !$result.durationInSeconds) {
        throw "Failed, skipped or untimed test: $($case.name)"
    }
    $timings[$file] += [double]::Parse($result.durationInSeconds, [cultureinfo]::InvariantCulture)
}
if ($pending.Count) { throw "Missing registered tests: $($pending -join ', ')" }
foreach ($file in $ExpectedFiles) {
    if (!$timings.ContainsKey($file)) { throw "No test ran from $file" }
}

$rows = foreach ($entry in $timings.GetEnumerator() | Sort-Object Name) {
    [pscustomobject]@{ file = $entry.Key; seconds = $entry.Value }
}
$report = [ordered]@{
    revision = $env:GITHUB_SHA
    seed = $executed.Catch2TestRun.'rng-seed'
    cases = @($executed.Catch2TestRun.TestCase).Count
    files = @($rows)
}
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output "Verified $($report.cases) cases in $($rows.Count) source files (seed $($report.seed))."
