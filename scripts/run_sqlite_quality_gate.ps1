param(
    [string]$BuildDir = "build_ci",
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repoRoot $BuildDir

Write-Host "==> Configuring SQLite-enabled build at $buildPath"
# CMAKE_BUILD_TYPE applies to single-config generators such as Ninja; Visual Studio uses --config.
cmake -S $repoRoot -B $buildPath "-DCMAKE_BUILD_TYPE=$Config" -DJISTATS_BUILD_TESTS=ON -DJISTATS_USE_SYSTEM_SQLITE=OFF
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }

Write-Host "==> Building $Config"
cmake --build $buildPath --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }

Write-Host "==> Running tests"
ctest --test-dir $buildPath -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "ctest failed with exit code $LASTEXITCODE" }
