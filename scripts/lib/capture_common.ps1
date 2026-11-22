function Get-EnvValue {
    param([string]$Name)

    $value = [Environment]::GetEnvironmentVariable($Name, "Process")
    if ([string]::IsNullOrEmpty($value)) { $value = [Environment]::GetEnvironmentVariable($Name, "User") }
    if ([string]::IsNullOrEmpty($value)) { $value = [Environment]::GetEnvironmentVariable($Name, "Machine") }
    return $value
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
    [System.IO.Directory]::CreateDirectory($OutputDir) | Out-Null
    return $OutputDir
}
