#pragma once

#include <windows.h>

#include <commctrl.h>
#include <uxtheme.h>

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

    bool usesHighContrast() const {
        return highContrast;
    }

    COLORREF color(COLORREF retroColor, int systemColor) const {
        return highContrast ? GetSysColor(systemColor) : retroColor;
    }

    COLORREF panelColor() const {
        return color(RGB(236, 233, 216), COLOR_BTNFACE);
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
        DrawEdge(dc, &rect, EDGE_RAISED, BF_RECT | BF_ADJUST);
        const int inset = MulDiv(2, GetDpiForWindow(draw.hwndItem), 96);
        if (selected && !highContrast) {
            const RECT accent{rect.left, rect.top, rect.right, rect.top + inset};
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
        if ((draw.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS) {
            InflateRect(&rect, -inset, -inset);
            DrawFocusRect(dc, &rect);
        }
        RestoreDC(dc, saved);
    }

  private:
    bool highContrast = false;

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
        if ((GetWindowLongPtrW(window, GWL_STYLE) & TCS_BUTTONS) == 0) {
            // The tab control still draws the frame and items. Its page fill would cover our panel.
            RECT pane{};
            GetClientRect(window, &pane);
            TabCtrl_AdjustRect(window, FALSE, &pane);
            ExcludeClipRect(dc, pane.left, pane.top, pane.right, pane.bottom);
        }
        DefSubclassProc(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        RestoreDC(dc, saved);
    }
};

} // namespace JiStats
