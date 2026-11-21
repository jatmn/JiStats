#pragma once

#ifdef _WIN32

#include <windows.h>

namespace JiStats {

class WindowsAppAssets {
public:
    // Caller owns the returned icon handle and must destroy it with DestroyIcon.
    static HICON loadAppIcon(int desiredSize);
};

} // namespace JiStats

#endif
