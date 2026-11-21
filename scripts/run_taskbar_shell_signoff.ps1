[CmdletBinding()]
param(
    [string]$ExePath = "build/bin/Release/JiStats.exe",
    [int]$MenuIterations = 3,
    [int]$StartupDelaySeconds = 5,
    [switch]$SkipExplorerRestart
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class JiStatsTaskbarWin32 {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr hWndParent, EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

    public static IntPtr MakeLParam(short lo, short hi) {
        return (IntPtr)(((int)hi << 16) | ((int)lo & 0xffff));
    }
}
'@

$MOUSEEVENTF_LEFTDOWN = 0x0002
$MOUSEEVENTF_LEFTUP = 0x0004
$MOUSEEVENTF_RIGHTDOWN = 0x0008
$MOUSEEVENTF_RIGHTUP = 0x0010

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Condition,
        [int]$TimeoutMs = 10000,
        [int]$PollIntervalMs = 100
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Condition) {
            return $true
        }
        Start-Sleep -Milliseconds $PollIntervalMs
    }

    return $false
}

function Get-ClassName {
    param([Parameter(Mandatory = $true)][IntPtr]$Handle)

    $builder = New-Object System.Text.StringBuilder 256
    [void][JiStatsTaskbarWin32]::GetClassName($Handle, $builder, $builder.Capacity)
    return $builder.ToString()
}

function Get-TaskbarHandle {
    return [JiStatsTaskbarWin32]::FindWindow("Shell_TrayWnd", $null)
}

function Get-VisibleMenuHandle {
    $script:__jistatsMenuHandle = [IntPtr]::Zero
    $callback = [JiStatsTaskbarWin32+EnumWindowsProc]{
        param($hWnd, $lParam)
        if ((Get-ClassName -Handle $hWnd) -eq "#32768" -and [JiStatsTaskbarWin32]::IsWindowVisible($hWnd)) {
            $script:__jistatsMenuHandle = $hWnd
            return $false
        }
        return $true
    }
    [void][JiStatsTaskbarWin32]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:__jistatsMenuHandle
}

function Get-TaskbarSurfaceHandle {
    $taskbar = Get-TaskbarHandle
    if ($taskbar -eq [IntPtr]::Zero) {
        return [IntPtr]::Zero
    }

    $script:__jistatsSurfaceHandle = [IntPtr]::Zero
    $callback = [JiStatsTaskbarWin32+EnumWindowsProc]{
        param($hWnd, $lParam)
        if ((Get-ClassName -Handle $hWnd) -eq "JiStatsTaskbarSurfaceWindow" -and
            [JiStatsTaskbarWin32]::IsWindowVisible($hWnd)) {
            $script:__jistatsSurfaceHandle = $hWnd
            return $false
        }
        return $true
    }
    [void][JiStatsTaskbarWin32]::EnumChildWindows($taskbar, $callback, [IntPtr]::Zero)
    return $script:__jistatsSurfaceHandle
}

function Get-WindowRectObject {
    param([Parameter(Mandatory = $true)][IntPtr]$Handle)

    $rect = New-Object JiStatsTaskbarWin32+RECT
    if (-not [JiStatsTaskbarWin32]::GetWindowRect($Handle, [ref]$rect)) {
        throw "Failed to query window rect for handle $Handle."
    }
    return $rect
}

function Invoke-TaskbarClick {
    param(
        [int]$X,
        [int]$Y
    )

    [void][JiStatsTaskbarWin32]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 120
    [JiStatsTaskbarWin32]::mouse_event($MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 50
    [JiStatsTaskbarWin32]::mouse_event($MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
}

function Get-TaskbarClickAwayPoint {
    param(
        [Parameter(Mandatory = $true)]$TaskbarRect,
        [Parameter(Mandatory = $true)]$SurfaceRect
    )

    $y = [int](($TaskbarRect.Top + $TaskbarRect.Bottom) / 2)
    $farLeftCandidate = $TaskbarRect.Left + 200
    if ($farLeftCandidate -lt ($SurfaceRect.Left - 32)) {
        return @{ X = $farLeftCandidate; Y = $y }
    }

    $leftCandidate = $SurfaceRect.Left - 48
    if ($leftCandidate -gt ($TaskbarRect.Left + 16)) {
        return @{ X = $leftCandidate; Y = $y }
    }

    $rightCandidate = $SurfaceRect.Right + 24
    if ($rightCandidate -lt ($TaskbarRect.Right - 16)) {
        return @{ X = $rightCandidate; Y = $y }
    }

    throw "Could not find a safe taskbar click-away point outside the JiStats surface."
}

function Open-OverlayContextMenu {
    param([Parameter(Mandatory = $true)][IntPtr]$SurfaceHandle)

    $rect = Get-WindowRectObject -Handle $SurfaceHandle
    $x = [int](($rect.Left + $rect.Right) / 2)
    $y = [int](($rect.Top + $rect.Bottom) / 2)

    [void][JiStatsTaskbarWin32]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 120
    [JiStatsTaskbarWin32]::mouse_event($MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 50
    [JiStatsTaskbarWin32]::mouse_event($MOUSEEVENTF_RIGHTUP, 0, 0, 0, [UIntPtr]::Zero)
}

function Assert-ContextMenuCycle {
    param([int]$Iteration)

    $surface = Get-TaskbarSurfaceHandle
    if ($surface -eq [IntPtr]::Zero) {
        throw "JiStats taskbar surface is not visible before menu cycle $Iteration."
    }

    $taskbarRect = Get-WindowRectObject -Handle (Get-TaskbarHandle)
    $surfaceRect = Get-WindowRectObject -Handle $surface
    $clickAway = Get-TaskbarClickAwayPoint -TaskbarRect $taskbarRect -SurfaceRect $surfaceRect

    Open-OverlayContextMenu -SurfaceHandle $surface
    if (-not (Wait-Until -TimeoutMs 5000 -Condition { (Get-VisibleMenuHandle) -ne [IntPtr]::Zero })) {
        throw "Context menu did not appear during cycle $Iteration."
    }

    Invoke-TaskbarClick -X $clickAway.X -Y $clickAway.Y
    if (-not (Wait-Until -TimeoutMs 5000 -Condition { (Get-VisibleMenuHandle) -eq [IntPtr]::Zero })) {
        throw "Context menu did not dismiss during cycle $Iteration."
    }

    if (-not (Wait-Until -TimeoutMs 5000 -Condition {
        $visibleSurface = Get-TaskbarSurfaceHandle
        $visibleSurface -ne [IntPtr]::Zero
    })) {
        throw "JiStats taskbar surface did not remain visible after click-away during cycle $Iteration."
    }
}

function Restart-ExplorerAndWaitForSurface {
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force

    Start-Sleep -Seconds 2
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
        Start-Process explorer.exe | Out-Null
    }

    if (-not (Wait-Until -TimeoutMs 30000 -Condition {
        (Get-TaskbarSurfaceHandle) -ne [IntPtr]::Zero
    })) {
        throw "JiStats taskbar surface did not return after Explorer restart."
    }
}

$exeFullPath = Resolve-Path $ExePath
$launchedProcess = $null
$initialSurface = [IntPtr]::Zero

try {
    Get-Process JiStats -ErrorAction SilentlyContinue | Stop-Process -Force

    $launchedProcess = Start-Process -FilePath $exeFullPath -ArgumentList "--background" -PassThru
    Start-Sleep -Seconds $StartupDelaySeconds

    if (-not (Wait-Until -TimeoutMs 15000 -Condition { (Get-TaskbarSurfaceHandle) -ne [IntPtr]::Zero })) {
        throw "JiStats taskbar surface did not appear."
    }

    $initialSurface = Get-TaskbarSurfaceHandle

    $menuResults = @()
    for ($iteration = 1; $iteration -le $MenuIterations; $iteration++) {
        Assert-ContextMenuCycle -Iteration $iteration
        $menuResults += "cycle-${iteration}:pass"
    }

    $explorerRestartResult = "skipped"
    if (-not $SkipExplorerRestart) {
        Restart-ExplorerAndWaitForSurface
        Assert-ContextMenuCycle -Iteration ($MenuIterations + 1)
        $explorerRestartResult = "pass"
    }

    [pscustomobject]@{
        exePath = $exeFullPath.Path
        pid = $launchedProcess.Id
        menuCycles = $menuResults
        explorerRestart = $explorerRestartResult
        initialSurfaceHandle = "$initialSurface"
        finalSurfaceHandle = "$(Get-TaskbarSurfaceHandle)"
    } | ConvertTo-Json -Depth 3
}
finally {
    if ($launchedProcess -and -not $launchedProcess.HasExited) {
        Stop-Process -Id $launchedProcess.Id -Force
    }
}
