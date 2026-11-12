#include "Theme.h"

namespace {
    bool g_dark = false;
    bool g_highContrast = false;

    bool ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD* out) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
        DWORD value = 0, size = sizeof(value), type = 0;
        const LSTATUS s = RegQueryValueExW(key, name, nullptr, &type,
                                          reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
        if (s != ERROR_SUCCESS || type != REG_DWORD) return false;
        *out = value;
        return true;
    }
}

void Theme::Refresh() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    g_highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                     (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;

    // 用户显式配置优先：0=跟随系统 1=浅色 2=深色
    DWORD mode = 0;
    if (!ReadDword(HKEY_CURRENT_USER, L"Software\\HashPage", L"Theme", &mode)) mode = 0;

    if (mode == 1) {
        g_dark = false;
    } else if (mode == 2) {
        g_dark = true;
    } else {
        // AppsUseLightTheme: 1=浅色 0=深色；取不到时按浅色处理
        DWORD light = 1;
        ReadDword(HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme", &light);
        g_dark = (light == 0);
    }

    // 高对比度下不使用自定义配色，全部交回系统
    if (g_highContrast) g_dark = false;
}

bool Theme::IsDark() { return g_dark; }
bool Theme::HighContrast() { return g_highContrast; }

COLORREF Theme::Bg() {
    if (g_highContrast) return GetSysColor(COLOR_3DFACE);
    return g_dark ? RGB(32, 32, 32) : GetSysColor(COLOR_3DFACE);
}

COLORREF Theme::Panel() {
    if (g_highContrast) return GetSysColor(COLOR_WINDOW);
    return g_dark ? RGB(43, 43, 43) : GetSysColor(COLOR_WINDOW);
}

COLORREF Theme::Text() {
    if (g_highContrast) return GetSysColor(COLOR_WINDOWTEXT);
    return g_dark ? RGB(238, 238, 238) : GetSysColor(COLOR_WINDOWTEXT);
}

COLORREF Theme::Dim() {
    if (g_highContrast) return GetSysColor(COLOR_GRAYTEXT);
    return g_dark ? RGB(160, 160, 160) : GetSysColor(COLOR_GRAYTEXT);
}

COLORREF Theme::Accent() {
    if (g_highContrast) return GetSysColor(COLOR_HIGHLIGHT);
    return RGB(0, 120, 215);
}
