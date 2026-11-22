[CmdletBinding()]
param(
    [string]$BuildDir = "build-clang",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$BuildType = "Debug",
    [switch]$FixFormat,
    [switch]$SkipFormat,
    [switch]$SkipTidy
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDirPath = Join-Path $repoRoot $BuildDir
$clangFormatConfigPath = Join-Path $repoRoot ".clang-format"
$clangTidyConfigPath = Join-Path $repoRoot ".clang-tidy"

Set-Location -LiteralPath $repoRoot

function Add-ToolPathIfPresent {
    param([Parameter(Mandatory = $true)][string]$PathToAdd)

    if ([string]::IsNullOrWhiteSpace($PathToAdd) -or -not (Test-Path -LiteralPath $PathToAdd)) {
        return $false
    }

    $pathEntries = ($env:PATH -split ';') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    if ($pathEntries -contains $PathToAdd) {
        return $true
    }

    $env:PATH = "$PathToAdd;$env:PATH"
    return $true
}

function Add-LocalLlvmToolPath {
    $candidates = [System.Collections.Generic.List[string]]::new()

    if ($env:JISTATS_LLVM_BIN) {
        $candidates.Add($env:JISTATS_LLVM_BIN)
    }

    $localPrograms = Join-Path $env:LOCALAPPDATA "Programs"
    if (Test-Path -LiteralPath $localPrograms) {
        $candidates.Add((Join-Path $localPrograms "LLVM\bin"))

        Get-ChildItem -LiteralPath $localPrograms -Directory -Filter "LLVM*" -ErrorAction SilentlyContinue |
            ForEach-Object {
                $directBin = Join-Path $_.FullName "bin"
                $candidates.Add($directBin)

                Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue |
                    ForEach-Object {
                        $nestedBin = Join-Path $_.FullName "bin"
                        $candidates.Add($nestedBin)
                    }
            }
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (-not (Add-ToolPathIfPresent -PathToAdd $candidate)) {
            continue
        }

        $hasFormat = Get-Command clang-format -ErrorAction SilentlyContinue
        $hasTidy = Get-Command clang-tidy -ErrorAction SilentlyContinue
        if ($hasFormat -and $hasTidy) {
            Write-Host ("Using LLVM tools from {0}" -f $candidate)
            return $true
        }
    }

    return $false
}

function Quote-CmdArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -notmatch '[\s&()<>^|"]') {
        return $Value
    }

    return '"' + $Value.Replace('"', '""') + '"'
}

function Get-VsDevCmdPath {
    $candidates = [System.Collections.Generic.List[string]]::new()

    if ($env:VSINSTALLDIR) {
        $candidates.Add((Join-Path $env:VSINSTALLDIR "Common7\Tools\VsDevCmd.bat"))
    }

    $vsWherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vsWherePath) {
        $installPath = & $vsWherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($installPath)) {
            $candidates.Add((Join-Path $installPath.Trim() "Common7\Tools\VsDevCmd.bat"))
        }
    }

    $commonRoots = @(
        # Visual Studio 2026 installs under the major-version "18" root.
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat"
    )
    foreach ($root in $commonRoots) {
        $candidates.Add($root)
    }

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    return $null
}

function Invoke-CmakeConfigure {
    $cmakeArgs = @(
        "-S", $repoRoot,
        "-B", $buildDirPath,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DJISTATS_BUILD_TESTS=ON",
        "-DJISTATS_ENABLE_UPX=OFF"
    )

    # Ninja can configure in an active MinGW or Visual Studio environment.
    if ($env:CXX -or (Get-Command g++ -ErrorAction SilentlyContinue) -or
        (Get-Command cl -ErrorAction SilentlyContinue)) {
        Write-Host ("Running: cmake {0}" -f ($cmakeArgs -join " "))
        & cmake @cmakeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed."
        }
        return
    }

    $vsDevCmdPath = Get-VsDevCmdPath
    if (-not $vsDevCmdPath) {
        throw "No C++ toolchain found. Run from an MSYS2 UCRT64 environment with GCC, set CC and CXX, or open a Visual Studio Developer PowerShell."
    }

    $quotedArgs = $cmakeArgs | ForEach-Object { Quote-CmdArgument $_ }
    $cmdLine = 'call {0} -no_logo && cmake {1}' -f (Quote-CmdArgument $vsDevCmdPath), ($quotedArgs -join " ")

    Write-Host ("Running through VsDevCmd: {0}" -f $cmdLine)
    & cmd.exe /d /s /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed through VsDevCmd."
    }
}

function Get-RepoFiles {
    param([switch]$TranslationUnitsOnly)

    $roots = @("src", "tests")
    $extensions = @(".c", ".cc", ".cpp", ".cxx")
    if (-not $TranslationUnitsOnly) {
        $roots = @("src", "include", "tests")
        $extensions += @(".h", ".hh", ".hpp", ".hxx")
    }

    foreach ($root in $roots) {
        $path = Join-Path $repoRoot $root
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }

        Get-ChildItem -LiteralPath $path -Recurse -File |
            Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
            Sort-Object FullName |
            ForEach-Object { $_.FullName }
    }
}

if (-not (Get-Command clang-format -ErrorAction SilentlyContinue) -or
    -not (Get-Command clang-tidy -ErrorAction SilentlyContinue)) {
    [void](Add-LocalLlvmToolPath)
}

$missingTools = [System.Collections.Generic.List[string]]::new()
if (-not $SkipFormat -and -not (Get-Command clang-format -ErrorAction SilentlyContinue)) {
    $missingTools.Add("clang-format")
}
if (-not $SkipTidy -and -not (Get-Command clang-tidy -ErrorAction SilentlyContinue)) {
    $missingTools.Add("clang-tidy")
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $missingTools.Add("cmake")
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $missingTools.Add("ninja")
}

if ($missingTools.Count -gt 0) {
    throw ("Missing required tool(s): {0}. Install them or add them to PATH before running this gate." -f (($missingTools | Select-Object -Unique) -join ", "))
}

Invoke-CmakeConfigure

$compileCommandsPath = Join-Path $buildDirPath "compile_commands.json"
if (-not (Test-Path -LiteralPath $compileCommandsPath)) {
    throw ("Expected compile database was not generated at {0}." -f $compileCommandsPath)
}

if (-not $SkipFormat) {
    if (-not (Test-Path -LiteralPath $clangFormatConfigPath)) {
        throw ("Missing clang-format config at {0}." -f $clangFormatConfigPath)
    }

    $formatFailures = [System.Collections.Generic.List[string]]::new()
    foreach ($file in Get-RepoFiles) {
        $formatArgs = @("--style=file")
        if ($FixFormat) {
            $formatArgs += "-i"
        } else {
            $formatArgs += @("--dry-run", "--Werror")
        }
        $formatArgs += $file

        & clang-format @formatArgs
        if ($LASTEXITCODE -ne 0) {
            $formatFailures.Add($file)
        }
    }

    if ($formatFailures.Count -gt 0) {
        throw ("clang-format reported issues in {0} file(s)." -f $formatFailures.Count)
    }
}

if (-not $SkipTidy) {
    if (-not (Test-Path -LiteralPath $clangTidyConfigPath)) {
        throw ("Missing clang-tidy config at {0}." -f $clangTidyConfigPath)
    }

    $tidyFailures = [System.Collections.Generic.List[string]]::new()
    foreach ($file in (Get-RepoFiles -TranslationUnitsOnly)) {
        & clang-tidy `
            "--config-file=$clangTidyConfigPath" `
            "-p" $buildDirPath `
            "--header-filter=($([regex]::Escape($repoRoot)))[\\/](src|include|tests)[\\/].*" `
            "--warnings-as-errors=*" `
            $file

        if ($LASTEXITCODE -ne 0) {
            $tidyFailures.Add($file)
        }
    }

    if ($tidyFailures.Count -gt 0) {
        throw ("clang-tidy reported issues in {0} translation unit(s)." -f $tidyFailures.Count)
    }
}

Write-Host "clang-format and clang-tidy completed successfully."
