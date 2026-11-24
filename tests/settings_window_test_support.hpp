#pragma once

#include <windows.h>

namespace JiStats::TestSupport {

inline HWND settingsViewport(HWND settings) {
    constexpr int kPreferencesViewportId = 1124;
    return GetDlgItem(settings, kPreferencesViewportId);
}

inline HWND settingsPage(HWND settings) {
    const HWND viewport = settingsViewport(settings);
    return viewport != nullptr ? GetWindow(viewport, GW_CHILD) : nullptr;
}

} // namespace JiStats::TestSupport
