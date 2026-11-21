# Run-on-Startup Behavior and Settings UX

> **Status**: Partially implemented
> **Purpose**: Define the intended startup behavior while noting what is already wired through `AppController`, `DashboardPreferences`, and `StartupManager`
> **Decision**: Opt-in (disabled by default)
> **Driven by**: plan.md section 10.4 (Startup and Recovery Direction) and section 12.3 (Settings)

## Decision: Opt-in Disabled by Default

**Selected**: "Start with Windows" is **OPT-IN**, disabled by default

**Rationale**:
1. A background utility that auto-starts may surprise users
2. Users should explicitly opt-in for a always-running background app
3. JiStats is not a critical system service
4. Users can still manually launch and it will run in background via tray

## Implementation Approach

### Windows Registry

Use Windows `Run` key for auto-start:
```
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
```
Value: `JiStats` = `C:\Path\To\JiStats.exe --background`

### Command-Line Flag

```cpp
// When started with --background flag:
// - Show tray icon only (no window)
// - Start minimized to tray
// - Begin sync operations immediately
// - Exit gracefully on user request
```

### Without --background flag:
```cpp
// Normal launch:
// - Show main window
// - Full UI startup
```

## Settings UX

### Location

Settings → General → "Start with Windows" checkbox

### UI Text

```
☐ Start with Windows
   When enabled, JiStats will start automatically when you sign in
   and run in the background
```

### States

| State | Checkbox | Behavior |
|-------|----------|----------|
| Default | Unchecked | Manual launch only |
| Enabled | Checked | Starts with Windows, minimizes to tray |
| Disabled | Unchecked | Stops auto-start |

### Settings Table Storage

From `docs/sqlite-schema.md` (app_settings seed data):
```sql
INSERT INTO app_settings (setting_key, setting_value) VALUES
    ('runtime.startup_enabled', 'false'),
    ('runtime.startup_background', 'true')
```

Current implementation note:

- the app currently persists startup preferences through `runtime.startup_enabled` and `runtime.startup_background`
- startup background behavior is already wired through `AppController` and `StartupManager`
- if startup is enabled and a background-mode rewrite fails, the existing registry `Run` entry remains the effective source of truth and the persisted preference is corrected to match it
- there is not currently a separate persisted `minimize_to_tray` or `show_window_on_start` setting

## Startup Flow

### Scenario 1: User has "Start with Windows" enabled

```
1. Windows starts, user logs in
2. Windows launches JiStats.exe
3. JiStats detects --background flag
4. App initializes in background mode:
   a. Load runtime_state
   b. Initialize tray icon
   c. Start connectivity check
   d. Begin sync scheduler
   e. Wait for sync completion
5. No main window shown
6. Tray icon visible, green dot (online)
```

### Scenario 2: User launches manually (normal)

```
1. User clicks JiStats shortcut/exe
2. No --background flag present
3. App initializes normally:
   a. Load runtime_state
   b. Show main window
   c. Initialize dashboard
   d. Start sync
   e. Show window with current data
```

### Scenario 3: User doesn't have auto-start enabled but app is running

```
1. User logs in, JiStats not in startup
2. User manually launches JiStats
3. JiStats starts normally with window
4. If already running, detect existing instance
5. Bring existing window to front
```

## Single Instance Enforcement

To prevent multiple instances:

```cpp
// Use named mutex or event
// If another instance exists, bring it to front and exit
HANDLE singleton = CreateMutex(NULL, TRUE, "JiStats_Singleton");
if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Another instance is running
    // Post message to bring it to front
    // Exit this instance
}
```

## Tray Behavior on Startup

### When Started with Windows (--background)

- Tray icon appears
- Tooltip: "JiStats - Online" or "JiStats - Syncing..."
- Double-click tray: opens main window
- Right-click tray: menu with "Open Dashboard", "Pause", "Exit"

### Tray Icon States (during auto-start)

| State | Icon | Tooltip |
|-------|------|---------|
| Starting | Gray gear | "JiStats - Starting..." |
| Online | Green dot | "JiStats - Online" |
| Syncing | Blue arrows | "JiStats - Syncing..." |
| Offline | Red dot | "JiStats - Offline" |
| Error | Yellow warning | "JiStats - Error" |

## Minimize to Tray vs Close

### Close button (X) behavior
- Option A: Minimize to tray (default for auto-start)
- Option B: Actually exit (option in settings)

**Decision**: Close minimizes to tray when:
- App was started via auto-start, OR
- "Minimize to tray" setting is enabled

### Exit from tray menu

"Exit" from tray menu actually closes the app completely.

## Lifecycle After Crash/Force Kill

If app was killed unexpectedly:
- On next launch, check `runtime_state.recovery_marker`
- Detect that last shutdown was abnormal
- Run recovery/ reconciliation as per gap-handling policy
- Show brief notice: "Recovering after unexpected shutdown"

## Settings Summary

### Relevant Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `runtime.startup_enabled` | bool | false | Run at Windows startup |
| `runtime.startup_background` | bool | true | When startup is enabled, launch with `--background` behavior |
| `ui.taskbar_overlay_enabled` | bool | true for current validation work | Separate taskbar validation preference, not a startup toggle |

### Settings UI Location

Settings → General:
```
Startup Behavior
────────────────
☑ Start with Windows
☑ Start in background when launched from Windows startup

[Save]
```

Current implementation note:

- close/minimize-to-tray behavior is currently handled by the app shell and tray/dashboard lifecycle, not by a separate startup-specific persisted setting

## Platform Implementation Notes

### Windows API Usage

```cpp
// Registry operations for startup
RegOpenKeyEx(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", ...);
// Set value: JiStats=C:\Path\To\JiStats.exe --background

// Or use SHGetValue for more robust registry access
```

### Shell Integration

- Use Windows startup folder alternative for simpler approach
- Or use registry as above (more standard)

Current implementation note:

- the shipped baseline uses the registry-backed `Run` entry through `StartupManager`

## User Feedback

When user enables "Start with Windows":
```
✓ JiStats will now start automatically when you sign in
  The app will run in the background and show in the system tray
```

When user disables:
```
✓ JiStats will no longer start automatically
  You can still launch it manually from the Start menu
```

## Edge Cases

### 1. User removes from startup but app running

- App continues running normally
- On next Windows restart, won't auto-start

### 2. User adds to startup while app running

- Setting applies immediately
- Takes effect on next Windows restart

### 3. App crash during first startup

- User may see crash dialog
- Next launch triggers recovery flow

### 4. Multiple Windows user accounts

- Each user has own registry settings
- Each user sees their own auto-start preference

## References

- Settings direction: `docs/plan.md` section 12.2
- Startup direction: `docs/plan.md` section 10.4
- Tray experience: `docs/plan.md` section 12.3
- App lifecycle: `docs/plan.md` section 10.2.2
