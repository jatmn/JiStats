param(
    [Parameter(Mandatory)][string] $Inventory,
    [Parameter(Mandatory)][string[]] $Results,
    [Parameter(Mandatory)][string] $Output,
    [Parameter(Mandatory)][string[]] $ExpectedFiles
)
$ErrorActionPreference = 'Stop'
[xml]$registered = Get-Content -Raw -LiteralPath $Inventory

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
$caseCount = 0
$seed = $null
foreach ($path in $Results) {
    [xml]$executed = Get-Content -Raw -LiteralPath $path
    $run = $executed.Catch2TestRun
    [uint32]$reportSeed = 0
    if (![uint32]::TryParse($run.'rng-seed', [Globalization.NumberStyles]::None,
                           [cultureinfo]::InvariantCulture, [ref]$reportSeed)) {
        throw "Missing or invalid RNG seed in $path"
    }
    if ($null -eq $seed) {
        $seed = $reportSeed
    } elseif ($seed -ne $reportSeed) {
        throw "RNG seed mismatch in $path (expected $seed, got $reportSeed)"
    }
    if (!$run.TestCase) { throw "No executed tests in $path" }

    foreach ($case in $run.TestCase) {
        $file = Get-SourcePath $case.filename
        if (!$pending.Remove("$file`n$($case.name)")) { throw "Unexpected or repeated test: $($case.name)" }
        $result = $case.OverallResult
        [double]$duration = 0
        if ($result.success -cne 'true' -or $result.skips -cne '0' -or
            ![double]::TryParse($result.durationInSeconds, [Globalization.NumberStyles]::Float,
                               [cultureinfo]::InvariantCulture, [ref]$duration) -or
            [double]::IsNaN($duration) -or [double]::IsInfinity($duration) -or $duration -lt 0) {
            throw "Failed, skipped or untimed test: $($case.name)"
        }
        $timings[$file] += $duration
        $caseCount++
    }
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
    seed = $seed.ToString([cultureinfo]::InvariantCulture)
    cases = $caseCount
    files = @($rows)
}
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output "Verified $($report.cases) cases in $($rows.Count) source files (seed $($report.seed))."
