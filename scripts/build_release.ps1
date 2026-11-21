[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string[]]$Target
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

$processNames = @(
    "JiStats",
    "JiStats-upx",
    "JiStatsMiniMaxCli",
    "JiStatsMiniMaxCli-upx",
    "JiStatsTests"
)

$runningProcesses = Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $processNames -contains $_.ProcessName }

if ($runningProcesses) {
    $summary = $runningProcesses |
        Sort-Object ProcessName, Id |
        ForEach-Object { "$($_.ProcessName)#$($_.Id)" }
    Write-Host ("Stopping running JiStats binaries before Release build: {0}" -f ($summary -join ", "))
    $runningProcesses | Stop-Process -Force
} else {
    Write-Host "No running JiStats binaries found."
}

$cmakeArgs = @("--build", $BuildDir, "--config", "Release")
if ($Target -and $Target.Count -gt 0) {
    $cmakeArgs += "--target"
    $cmakeArgs += $Target
}

Write-Host ("Running: cmake {0}" -f ($cmakeArgs -join " "))
& cmake @cmakeArgs
exit $LASTEXITCODE
