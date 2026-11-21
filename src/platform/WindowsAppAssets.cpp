#include "platform/WindowsAppAssets.hpp"

#ifdef _WIN32

#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")

namespace JiStats {

namespace {

constexpr int kJiStatsAppIconResourceId = 101;

class GdiPlusScope {
public:
    GdiPlusScope() {
        Gdiplus::GdiplusStartupInput startupInput;
        started_ = (Gdiplus::GdiplusStartup(&token_, &startupInput, nullptr) == Gdiplus::Ok);
    }

    ~GdiPlusScope() {
        if (started_) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    bool started() const {
        return started_;
    }

private:
    ULONG_PTR token_ = 0;
    bool started_ = false;
};

std::wstring executableDirectory() {
    wchar_t path[MAX_PATH] = L"";
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"";
    }

    if (!PathRemoveFileSpecW(path)) {
        return L"";
    }
    return path;
}

std::wstring joinPath(const std::wstring& base, const wchar_t* child) {
    std::wstring path = base;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += child;
    return path;
}

bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring findAssetPath(const wchar_t* fileName) {
    const std::wstring exeDir = executableDirectory();
    const std::vector<std::wstring> candidates = {
        joinPath(exeDir, fileName),
        joinPath(joinPath(exeDir, L"assets"), fileName),
        joinPath(joinPath(exeDir, L".."), fileName),
        joinPath(joinPath(joinPath(exeDir, L".."), L"assets"), fileName),
        joinPath(joinPath(exeDir, L"..\\.."), fileName),
        joinPath(joinPath(joinPath(exeDir, L"..\\.."), L"assets"), fileName),
        joinPath(joinPath(exeDir, L"..\\..\\.."), fileName)
    };

    for (const auto& candidate : candidates) {
        if (fileExists(candidate)) {
            return candidate;
        }
    }
    return L"";
}

std::unique_ptr<Gdiplus::Bitmap> loadBitmapFromPath(const std::wstring& path) {
    if (path.empty()) {
        return nullptr;
    }

    auto bitmap = std::make_unique<Gdiplus::Bitmap>(path.c_str());
    if (bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    return bitmap;
}

Gdiplus::Rect computeOpaqueBounds(Gdiplus::Bitmap& source) {
    const UINT width = source.GetWidth();
    const UINT height = source.GetHeight();
    UINT minX = width;
    UINT minY = height;
    UINT maxX = 0;
    UINT maxY = 0;
    bool found = false;

    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            Gdiplus::Color pixel;
            if (source.GetPixel(x, y, &pixel) != Gdiplus::Ok) {
                continue;
            }
            if (pixel.GetAlpha() <= 16) {
                continue;
            }

            found = true;
            minX = (std::min)(minX, x);
            minY = (std::min)(minY, y);
            maxX = (std::max)(maxX, x);
            maxY = (std::max)(maxY, y);
        }
    }

    if (!found) {
        return Gdiplus::Rect(0, 0,
                             static_cast<INT>(width),
                             static_cast<INT>(height));
    }

    return Gdiplus::Rect(static_cast<INT>(minX),
                         static_cast<INT>(minY),
                         static_cast<INT>(maxX - minX + 1),
                         static_cast<INT>(maxY - minY + 1));
}

HICON createScaledIcon(Gdiplus::Bitmap& source, int desiredSize) {
    Gdiplus::Bitmap scaled(desiredSize, desiredSize, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&scaled);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const Gdiplus::Rect sourceBounds = computeOpaqueBounds(source);
    const int contentSize = (std::max)(sourceBounds.Width, sourceBounds.Height);
    const int paddedSize = (std::max)(contentSize, 1);
    const int inset = (std::max)(1, desiredSize / 12);
    const int targetSize = (std::max)(1, desiredSize - (inset * 2));

    Gdiplus::RectF destination(static_cast<Gdiplus::REAL>(inset),
                               static_cast<Gdiplus::REAL>(inset),
                               static_cast<Gdiplus::REAL>(targetSize),
                               static_cast<Gdiplus::REAL>(targetSize));
    Gdiplus::RectF sourceSquare(static_cast<Gdiplus::REAL>(sourceBounds.X - ((paddedSize - sourceBounds.Width) / 2.0f)),
                                static_cast<Gdiplus::REAL>(sourceBounds.Y - ((paddedSize - sourceBounds.Height) / 2.0f)),
                                static_cast<Gdiplus::REAL>(paddedSize),
                                static_cast<Gdiplus::REAL>(paddedSize));
    graphics.DrawImage(&source, destination, sourceSquare.X, sourceSquare.Y, sourceSquare.Width, sourceSquare.Height, Gdiplus::UnitPixel);

    HICON icon = nullptr;
    if (scaled.GetHICON(&icon) != Gdiplus::Ok) {
        return nullptr;
    }
    return icon;
}

} // namespace

HICON WindowsAppAssets::loadAppIcon(int desiredSize) {
    if (desiredSize <= 0) {
        return nullptr;
    }

    HICON resourceIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(kJiStatsAppIconResourceId),
        IMAGE_ICON,
        desiredSize,
        desiredSize,
        LR_DEFAULTCOLOR));
    if (resourceIcon != nullptr) {
        return resourceIcon;
    }

    GdiPlusScope gdiplus;
    if (!gdiplus.started()) {
        return nullptr;
    }

    auto bitmap = loadBitmapFromPath(findAssetPath(L"icon2.png"));
    if (!bitmap) {
        return nullptr;
    }

    return createScaledIcon(*bitmap, desiredSize);
}

} // namespace JiStats

#endif
