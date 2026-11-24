[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string[]]$Target
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

$buildPath = if ($BuildDir) { (Resolve-Path -LiteralPath $BuildDir -ErrorAction SilentlyContinue).ProviderPath }
$cachePath = if ($buildPath) { Join-Path $buildPath "CMakeCache.txt" }
if ($cachePath -and (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    $cache = @{}
    Get-Content -LiteralPath $cachePath -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_ -match '^(CMAKE_[A-Z0-9_]+):[^=]+=(.*)$') {
            $cache[$Matches[1]] = $Matches[2]
        }
    }

    $executableTargets = @("JiStats", "JiStatsMiniMaxCli", "JiStatsTests")
    $installBuildsAll = $Target -contains "install" -and
        $cache["CMAKE_SKIP_INSTALL_ALL_DEPENDENCY"] -notmatch '^(1|ON|YES|TRUE|Y)$'
    if ($Target -and $Target -notcontains "all" -and $Target -notcontains "ALL_BUILD" -and -not $installBuildsAll) {
        $executableTargets = $executableTargets | Where-Object { $Target -contains $_ }
    }

    $multiConfig = [bool]$cache["CMAKE_CONFIGURATION_TYPES"]
    $config = if ($multiConfig) { "Release" } else { $cache["CMAKE_BUILD_TYPE"] }
    [string]$configuredOutput = if ($config) {
        $cache["CMAKE_RUNTIME_OUTPUT_DIRECTORY_$($config.ToUpperInvariant())"]
    }
    $defaultOutput = Join-Path $buildPath "bin"
    if ($multiConfig) {
        $defaultOutput = Join-Path $defaultOutput "Release"
    }

    # CMake evaluates generator expressions while generating the build.
    if ($executableTargets -and $configuredOutput -notmatch '\$<') {
        $outputExecutables = foreach ($executableTarget in $executableTargets) {
            $outputPath = $defaultOutput
            if ($configuredOutput) {
                $outputPath = $configuredOutput
                if (-not [IO.Path]::IsPathRooted($outputPath)) {
                    $targetBuildSubdir = if ($executableTarget -eq "JiStatsTests") { "tests" } else { "src" }
                    $outputPath = Join-Path (Join-Path $buildPath $targetBuildSubdir) $outputPath
                }
                $outputPath = [IO.Path]::GetFullPath($outputPath)
            }
            Join-Path $outputPath "$executableTarget.exe"
            if ($executableTarget -eq "JiStats") {
                Join-Path $outputPath "JiStats-upx.exe"
            }
        }
        $processNames = $outputExecutables | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }
        Get-Process -Name $processNames -ErrorAction SilentlyContinue | ForEach-Object {
            $executable = $_.Path
            if ($outputExecutables -contains $executable) {
                Write-Warning "Running build output '$executable' (PID $($_.Id)). Exit it normally if it blocks the build."
            }
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
