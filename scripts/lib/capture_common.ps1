function Get-EnvValue {
    param([string]$Name)

    $value = [Environment]::GetEnvironmentVariable($Name, "Process")
    if ([string]::IsNullOrEmpty($value)) { $value = [Environment]::GetEnvironmentVariable($Name, "User") }
    if ([string]::IsNullOrEmpty($value)) { $value = [Environment]::GetEnvironmentVariable($Name, "Machine") }
    return $value
}

function Resolve-CaptureDirectoryPath {
    param([string]$Path, [int]$RemainingLinks = 32)

    if ($RemainingLinks -lt 0) {
        throw "Capture directory contains too many links."
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $physicalPath = [System.IO.Path]::GetPathRoot($fullPath)
    $segments = $fullPath.Substring($physicalPath.Length).Split(
        [char[]]@([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar),
        [StringSplitOptions]::RemoveEmptyEntries)
    foreach ($segment in $segments) {
        $physicalPath = [System.IO.Path]::Combine($physicalPath, $segment)
        try {
            $item = Get-Item -LiteralPath $physicalPath -Force -ErrorAction Stop
        }
        catch [System.Management.Automation.ItemNotFoundException] {
            continue
        }

        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            $target = $item.Target
            if (-not $target) {
                throw "Cannot resolve capture directory link: $physicalPath"
            }
            if (-not [System.IO.Path]::IsPathRooted($target)) {
                $target = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($physicalPath), $target)
            }
            $physicalPath = Resolve-CaptureDirectoryPath -Path $target -RemainingLinks ($RemainingLinks - 1)
        }
    }
    return $physicalPath
}

function Initialize-CaptureDirectory {
    param(
        [string]$OutputDir,
        [string]$DefaultSubdirectory,
        [string]$RepoRoot
    )

    if ([string]::IsNullOrWhiteSpace($OutputDir)) {
        $localAppData = [Environment]::GetFolderPath('LocalApplicationData')
        $OutputDir = Join-Path $localAppData "JiStats/captures/$DefaultSubdirectory"
    }
    if (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
        $OutputDir = Join-Path $RepoRoot $OutputDir
    }
    $repoPath = Resolve-CaptureDirectoryPath -Path $RepoRoot
    $capturePath = Resolve-CaptureDirectoryPath -Path $OutputDir
    $separator = [System.IO.Path]::DirectorySeparatorChar
    $repoPath = $repoPath.TrimEnd($separator)
    $comparison = [StringComparison]::Ordinal
    if ($separator -eq [char]92) {
        $comparison = [StringComparison]::OrdinalIgnoreCase
    }
    if ($capturePath.Equals($repoPath, $comparison) -or $capturePath.StartsWith($repoPath + $separator, $comparison)) {
        throw "Capture output must be outside the JiStats source checkout."
    }
    [System.IO.Directory]::CreateDirectory($capturePath) | Out-Null
    return $capturePath
}
