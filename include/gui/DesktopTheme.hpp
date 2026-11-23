#pragma once

#include <windows.h>

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <iterator>
#include <optional>

namespace JiStats {

class DesktopTheme {
  public:
    void refresh() {
        HIGHCONTRASTW settings{};
        settings.cbSize = sizeof(settings);
        highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(settings), &settings, 0) &&
                       (settings.dwFlags & HCF_HIGHCONTRASTON);
    }

    void refreshWindow(HWND window, UINT message) {
        refresh();
        if (message == WM_SYSCOLORCHANGE) {
            EnumChildWindows(window, notifySystemColorChange, 0);
        }
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    bool usesHighContrast() const {
        return highContrast;
    }

    COLORREF color(COLORREF retroColor, int systemColor) const {
        return highContrast ? GetSysColor(systemColor) : retroColor;
    }

    COLORREF panelColor() const {
        return color(RGB(246, 248, 251), COLOR_BTNFACE);
    }

    COLORREF accentColor() const {
        return color(RGB(20, 68, 154), COLOR_HIGHLIGHT);
    }

    static void fill(HDC dc, const RECT& rect, COLORREF color) {
        const auto previous = SetDCBrushColor(dc, color);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc, previous);
    }

    LRESULT paintPanel(HWND window, HDC dc) const {
        RECT client{};
        GetClientRect(window, &client);
        fill(dc, client, panelColor());
        return TRUE;
    }

    std::optional<LRESULT> panelMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) const {
        const auto dc = reinterpret_cast<HDC>(wParam);
        if (message == WM_ERASEBKGND || message == WM_PRINTCLIENT) {
            return paintPanel(window, dc);
        }
        if (message != WM_CTLCOLORSTATIC && message != WM_CTLCOLORBTN) {
            return std::nullopt;
        }
        const auto control = reinterpret_cast<HWND>(lParam);
        if (message == WM_CTLCOLORSTATIC) {
            // Disabled and read-only edits request a static brush as well.
            wchar_t controlClass[16]{};
            GetClassNameW(control, controlClass, static_cast<int>(std::size(controlClass)));
            if (lstrcmpiW(controlClass, L"Edit") == 0) {
                return std::nullopt;
            }
        }
        const COLORREF background = panelColor();
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, background);
        SetTextColor(dc, GetSysColor(IsWindowEnabled(control) ? COLOR_BTNTEXT : COLOR_GRAYTEXT));
        SetDCBrushColor(dc, background);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }

    void styleTab(HWND tab) const {
        // Keep the common control's layout and navigation, with a classic pane border.
        SetWindowTheme(tab, L"", L"");
        SetWindowSubclass(tab, tabBackgroundProc, 0, reinterpret_cast<DWORD_PTR>(this));
    }

    void drawTab(const DRAWITEMSTRUCT& draw) const {
        const bool selected = static_cast<int>(draw.itemID) == TabCtrl_GetCurSel(draw.hwndItem);
        const HDC dc = draw.hDC;
        RECT rect = draw.rcItem;
        const int saved = SaveDC(dc);
        if (saved == 0) {
            return;
        }
        fill(dc, rect, selected ? color(RGB(255, 255, 255), COLOR_HIGHLIGHT) : panelColor());
        const UINT dpi = GetDpiForWindow(draw.hwndItem);
        const int inset = MulDiv(2, dpi, 96);
        LONG accentBottom = rect.top;
        if (selected && !highContrast) {
            // Native tabs can clip the outer edge of rcItem from the owner-draw DC.
            const LONG accentTop = rect.top + GetSystemMetricsForDpi(SM_CYEDGE, dpi);
            accentBottom = std::min(rect.bottom, accentTop + inset);
            const RECT accent{rect.left, accentTop, rect.right, accentBottom};
            fill(dc, accent, RGB(232, 157, 43));
        }
        wchar_t label[64]{};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = label;
        item.cchTextMax = static_cast<int>(std::size(label));
        SendMessageW(draw.hwndItem, TCM_GETITEMW, draw.itemID, reinterpret_cast<LPARAM>(&item));
        if (const auto font = SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0)) {
            SelectObject(dc, reinterpret_cast<HFONT>(font));
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(selected && highContrast ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT));
        DrawTextW(dc, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (GetFocus() == draw.hwndItem && (draw.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) {
            InflateRect(&rect, -inset, -inset);
            rect.top = std::max(rect.top, accentBottom);
            DrawFocusRect(dc, &rect);
        }
        RestoreDC(dc, saved);
    }

  private:
    bool highContrast = false;

    static BOOL CALLBACK notifySystemColorChange(HWND child, LPARAM) {
        SendMessageW(child, WM_SYSCOLORCHANGE, 0, 0);
        wchar_t controlClass[32]{};
        GetClassNameW(child, controlClass, static_cast<int>(std::size(controlClass)));
        if (lstrcmpiW(controlClass, WC_LISTVIEWW) == 0) {
            // List views cache these data-surface colors independently of the system notification.
            const COLORREF background = GetSysColor(COLOR_WINDOW);
            ListView_SetBkColor(child, background);
            ListView_SetTextBkColor(child, background);
            ListView_SetTextColor(child, GetSysColor(COLOR_WINDOWTEXT));
        }
        return TRUE;
    }

    static LRESULT CALLBACK tabBackgroundProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                              DWORD_PTR context) {
        const auto& theme = *reinterpret_cast<const DesktopTheme*>(context);
        switch (message) {
            case WM_ERASEBKGND:
                return TRUE;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const HDC dc = BeginPaint(window, &paint);
                theme.paintTab(window, dc);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_PRINTCLIENT:
                theme.paintTab(window, reinterpret_cast<HDC>(wParam));
                return 0;
            case WM_NCDESTROY:
                RemoveWindowSubclass(window, tabBackgroundProc, id);
                break;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void paintTab(HWND window, HDC dc) const {
        paintPanel(window, dc);
        const int saved = SaveDC(dc);
        if (saved == 0) {
            return;
        }
        // The tab control still draws its frame and items. Its page fill would cover our panel.
        RECT pane{};
        GetClientRect(window, &pane);
        TabCtrl_AdjustRect(window, FALSE, &pane);
        ExcludeClipRect(dc, pane.left, pane.top, pane.right, pane.bottom);
        DefSubclassProc(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        RestoreDC(dc, saved);
    }
};

} // namespace JiStats
