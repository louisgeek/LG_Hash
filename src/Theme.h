#pragma once

#include <windows.h>

// 深色模式支持。不调用 uxtheme 的未公开序号导出（那些在不同 Windows 版本间会变），
// 只用「读注册表判断 + 自己画颜色」，行为确定、不会影响宿主 explorer 进程的其它窗口。
namespace Theme {
    // HKCU\Software\LG_Hash\Theme: 0=跟随系统 1=浅色 2=深色
    // 先读配置，再读系统 AppsUseLightTheme；高对比度模式下强制走系统配色。
    void Refresh();

    bool IsDark();
    bool HighContrast();

    COLORREF Bg();      // 页面背景
    COLORREF Panel();   // 列表 / 输入域背景
    COLORREF Text();    // 主文字
    COLORREF Dim();     // 次要文字
    COLORREF Accent();  // 进度条、强调色
}
