[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string[]]$Target
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

$executableTargets = @("JiStats", "JiStatsMiniMaxCli", "JiStatsTests")
if ($Target -and $Target -notcontains "all" -and $Target -notcontains "ALL_BUILD") {
    $executableTargets = $executableTargets | Where-Object { $Target -contains $_ }
}

$buildPath = (Resolve-Path -LiteralPath $BuildDir -ErrorAction SilentlyContinue).ProviderPath
$cachePath = if ($buildPath) { Join-Path $buildPath "CMakeCache.txt" }
if ($executableTargets -and $cachePath -and (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    $outputPath = Join-Path $buildPath "bin"
    if (Select-String -LiteralPath $cachePath -Pattern '^CMAKE_CONFIGURATION_TYPES:[^=]+=.+' -Quiet -ErrorAction SilentlyContinue) {
        $outputPath = Join-Path $outputPath "Release"
    }
    $processNames = foreach ($executableTarget in $executableTargets) {
        $executableTarget
        if ($executableTarget -eq "JiStats") {
            "JiStats-upx"
        }
    }
    $outputExecutables = $processNames | ForEach-Object { Join-Path $outputPath "$_.exe" }

    Get-Process -Name $processNames -ErrorAction SilentlyContinue | ForEach-Object {
        $executable = $_.Path
        if ($outputExecutables -contains $executable) {
            Write-Warning "Running build output '$executable' (PID $($_.Id)). Exit it normally if it blocks the build."
        }
    }
}

$cmakeArgs = @("--build", $BuildDir, "--config", "Release")
if ($Target -and $Target.Count -gt 0) {
    $cmakeArgs += "--target"
    $cmakeArgs += $Target
}

Write-Host ("Running: cmake {0}" -f ($cmakeArgs -join " "))
& cmake @cmakeArgs
exit $LASTEXITCODE
