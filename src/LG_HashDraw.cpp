
// 列表渲染与全部自绘代码：列头、按钮、菜单、占位提示、语言套用。
// 深色模式下这些控件都要自己画——系统主题不跟随页面配色。

#include "LG_HashInternal.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <string>
#include <vector>

#include "Lang.h"
#include "Theme.h"
#include "resource.h"


// 大小写切换时从已存结果重渲染，不重读文件
static std::wstring ApplyCase(const std::wstring& hex, bool upper) {
    if (!upper) return hex;
    std::wstring s = hex;
    for (auto& c : s)
        if (c >= L'a' && c <= L'f') c -= L'a' - L'A';
    return s;
}

// 按勾选状态重建列表：未勾选的算法直接不出现在列表里（而不是显示 —）。
// placeholder 非空时所有行的值列填占位符（初始「—」/ 计算中「…」）；
// 为空时按完成状态填真实值（未算出的算法仍显示 —，提示重算可取回）。
void RebuildList(HWND hwnd, PageState* st, const wchar_t* placeholder) {
    HWND list = GetDlgItem(hwnd, IDC_HASHLIST);
    ListView_DeleteAllItems(list);

    ResultSnap snap;
    Job* job = st->ctx->job;
    const bool haveJob = job && st->cur < job->paths.size();
    if (haveJob) SnapCurrent(job, st->cur, &snap);

    int row = 0;
    for (int i = 0; i < kHashCount; ++i) {
        if (!st->algo[i]) continue;
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = const_cast<wchar_t*>(kHashAlgos[i].name);
        ListView_InsertItem(list, &it);

        std::wstring value = L"—";
        if (placeholder) {
            value = placeholder;
        } else if (haveJob && snap.finished && !snap.hex[i].empty()) {
            value = ApplyCase(snap.hex[i], st->upper);
        }
        ListView_SetItemText(list, row, 1, value.data());
        ++row;
    }
    // 行重建后选中状态丢失，同步「复制选中」的可用性
    UpdateCopySelState(hwnd, st);
}

void RenderCurrent(HWND hwnd, PageState* st) {
    RebuildList(hwnd, st, nullptr);
}

// ---------------------------------------------------------------- 弹出菜单

// 菜单字体：首次使用时从系统菜单字体缓存到页面状态
HFONT EnsureMenuFont(PageState* st) {
    if (!st->menuFont) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            st->menuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    }
    return st->menuFont;
}

// 弹出统一自绘的菜单。items 指向的文本必须在 TrackPopupMenu 返回前有效，
// 因此调用方一律用栈数组 / 局部 vector。
static void ShowPopupMenu(HWND hwnd, PageState* st, MenuEntry* items, int count, POINT pt) {
    EnsureMenuFont(st);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (int i = 0; i < count; ++i) {
        // MF_OWNERDRAW 的 lpNewItem 即 WM_MEASUREITEM/WM_DRAWITEM 的 itemData
        AppendMenuW(menu, MF_OWNERDRAW, items[i].id,
                    reinterpret_cast<LPCWSTR>(&items[i]));
    }
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// 自绘单个菜单项
void DrawMenuEntry(const DRAWITEMSTRUCT* dis, HFONT font) {
    auto e = reinterpret_cast<const MenuEntry*>(dis->itemData);
    if (!e) return;
    const bool dark = Theme::IsDark();
    const bool hot = (dis->itemState & ODS_SELECTED) != 0;

    // 底色：深色跟主题，浅色跟系统菜单
    const COLORREF bg = dark ? (hot ? RGB(62, 62, 62) : Theme::Bg())
                             : GetSysColor(hot ? COLOR_HIGHLIGHT : COLOR_MENU);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, b);
    DeleteObject(b);

    if (!e->text) {   // 分隔线
        RECT r = dis->rcItem;
        r.top = (r.top + r.bottom) / 2;
        r.bottom = r.top + 1;
        r.left += 10;
        r.right -= 10;
        HBRUSH line = CreateSolidBrush(dark ? RGB(72, 72, 72) : GetSysColor(COLOR_BTNSHADOW));
        FillRect(dis->hDC, &r, line);
        DeleteObject(line);
        return;
    }

    const COLORREF fg = dark ? Theme::Text()
                             : GetSysColor(hot ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    HGDIOBJ oldFont = font ? SelectObject(dis->hDC, font) : nullptr;

    // 左侧勾选列
    if (e->checked) {
        RECT ck = dis->rcItem;
        ck.left += 7;
        ck.right = ck.left + 14;
        DrawTextW(dis->hDC, L"\u2713", -1, &ck,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    RECT rc = dis->rcItem;
    rc.left += 24;
    rc.right -= 8;
    DrawTextW(dis->hDC, e->text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // 下划线：文字底部，宽度与文字等宽
    if (e->underline) {
        SIZE sz{};
        if (GetTextExtentPoint32W(dis->hDC, e->text, static_cast<int>(wcslen(e->text)), &sz)) {
            const int mid = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            const int y = mid + sz.cy / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, fg);
            HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
            MoveToEx(dis->hDC, rc.left, y, nullptr);
            LineTo(dis->hDC, rc.left + sz.cx, y);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(pen);
        }
    }

    if (oldFont) SelectObject(dis->hDC, oldFont);
}

// 多选时的文件跳转菜单（IDC_FILESEL）：列出全部文件，勾选当前项
void ShowFilesMenu(HWND hwnd, PageState* st) {
    const size_t n = st->ctx->paths.size();
    if (n <= 1) return;

    std::vector<std::wstring> names;
    names.reserve(n);
    for (const auto& p : st->ctx->paths)
        names.emplace_back(PathFindFileNameW(p.c_str()));

    std::vector<MenuEntry> items;
    items.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        items.push_back(MenuEntry{ names[i].c_str(),
                                   static_cast<UINT>(IDM_FILE_BASE + i),
                                   i == st->cur, false });
    }

    RECT rc{};
    GetWindowRect(GetDlgItem(hwnd, IDC_FILESEL), &rc);
    const POINT pt{ rc.left, rc.bottom };
    ShowPopupMenu(hwnd, st, items.data(), static_cast<int>(items.size()), pt);
}

// 项目主页
void OpenProjectPage(HWND hwnd) {
    // ShellExecute 不需要可写命令行缓冲，也无 PATH_MAX 限制
    ShellExecuteW(hwnd, L"open",
                  L"https://github.com/louisgeek/LG_Hash",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- 外观

// 深色下列头也要自绘：header 是独立控件，ListView 的颜色设置管不到它
LRESULT CustomDrawHeader(LPNMCUSTOMDRAW cd) {
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        // 先把整个列头区域刷成主题色，避免未覆盖处露出系统浅色
        RECT all = cd->rc;
        HBRUSH bg = CreateSolidBrush(Theme::Bg());
        FillRect(cd->hdc, &all, bg);
        DeleteObject(bg);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        RECT rc = cd->rc;
        HBRUSH bg = CreateSolidBrush(Theme::Bg());
        FillRect(cd->hdc, &rc, bg);
        DeleteObject(bg);

        // 细分隔线，避免深色下一片糊在一起
        HPEN pen = CreatePen(PS_SOLID, 1, Theme::IsDark() ? RGB(70, 70, 70) : GetSysColor(COLOR_BTNSHADOW));
        HGDIOBJ oldPen = SelectObject(cd->hdc, pen);
        MoveToEx(cd->hdc, rc.right - 1, rc.top + 2, nullptr);
        LineTo(cd->hdc, rc.right - 1, rc.bottom - 2);
        SelectObject(cd->hdc, oldPen);
        DeleteObject(pen);

        wchar_t text[128] = {};
        // 列号即 item 序号，文字用当前语言表
        if (cd->dwItemSpec == 0)
            wcscpy_s(text, Lang::Get(Lang::Id::AlgoColumn));
        else
            wcscpy_s(text, Lang::Get(Lang::Id::HashColumn));

        SetBkMode(cd->hdc, TRANSPARENT);
        SetTextColor(cd->hdc, Theme::Dim());
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(cd->hdr.hwndFrom, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = font ? SelectObject(cd->hdc, font) : nullptr;
        RECT tr = rc;
        tr.left += 6;
        DrawTextW(cd->hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (oldFont) SelectObject(cd->hdc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    default:
        break;
    }
    return CDRF_DODEFAULT;
}

// 校验框占位提示：部分系统构建不支持 EM_SETCUEBANNER（实测返回 0），
// 这里用「空态时把提示文字放进输入框」的方式等价实现：
// 聚焦时清空，失焦且为空时恢复，配色走 WM_CTLCOLOREDIT 的灰色分支。
void SetVerifyCue(HWND hwnd, PageState* st, bool show) {
    HWND edit = GetDlgItem(hwnd, IDC_VERIFY);
    if (!edit) return;
    // 必须先置标志再改文本：SetWindowText 会同步触发 EN_CHANGE，
    // 若标志还是旧值，占位文字会被当成用户输入去校验，误显示「不匹配」
    st->cueShown = show;
    SetWindowTextW(edit, show ? Lang::Get(Lang::Id::VerifyCue) : L"");
}

void ApplyLanguage(HWND hwnd) {
    SetDlgItemTextW(hwnd, IDC_VERIFYGROUP, Lang::Get(Lang::Id::VerifyGroup));
    SetDlgItemTextW(hwnd, IDC_COPYSEL, Lang::Get(Lang::Id::CopySelected));
    SetDlgItemTextW(hwnd, IDC_COPYALL, Lang::Get(Lang::Id::CopyAll));
    SetDlgItemTextW(hwnd, IDC_EXPORT, Lang::Get(Lang::Id::Export));
    SetDlgItemTextW(hwnd, IDC_SETTINGS_BTN, Lang::Get(Lang::Id::SettingsTitle));
    SetDlgItemTextW(hwnd, IDC_PREV, Lang::Get(Lang::Id::PrevFile));
    SetDlgItemTextW(hwnd, IDC_NEXT, Lang::Get(Lang::Id::NextFile));
    // IDC_REHASH 的文案随状态变化（计算 / 取消 / 重新计算），
    // 由 WM_INITDIALOG 与 StartJob/OnDone 分别设置，这里不碰，避免闪一下错字。
    HWND list = GetDlgItem(hwnd, IDC_HASHLIST);
    if (list) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT;
        col.pszText = const_cast<wchar_t*>(Lang::Get(Lang::Id::AlgoColumn));
        ListView_SetColumn(list, 0, &col);
        col.pszText = const_cast<wchar_t*>(Lang::Get(Lang::Id::HashColumn));
        ListView_SetColumn(list, 1, &col);
    }
}

// 给按钮加上 BS_OWNERDRAW，使其改由 DrawOwnerButton 绘制。
// 只在深色模式下这么做：浅色模式交给系统主题，能和宿主属性表保持一致。
void EnableOwnerDraw(HWND page) {
    for (int id : { IDC_COPYSEL, IDC_COPYALL, IDC_EXPORT, IDC_REHASH,
                    IDC_PREV, IDC_NEXT, IDC_FILESEL, IDC_SETTINGS_BTN }) {
        HWND btn = GetDlgItem(page, id);
        if (!btn) continue;
        const LONG_PTR style = GetWindowLongPtrW(btn, GWL_STYLE);
        if (style & BS_OWNERDRAW) continue;
        SetWindowLongPtrW(btn, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
        InvalidateRect(btn, nullptr, TRUE);
    }
}

// 深色模式下按钮走 owner-draw：直接用系统主题画会得到浅色按钮压在深色底上。
LRESULT DrawOwnerButton(const DRAWITEMSTRUCT* dis) {
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    COLORREF face = Theme::IsDark() ? RGB(58, 58, 58) : GetSysColor(COLOR_BTNFACE);
    COLORREF edge = Theme::IsDark() ? RGB(90, 90, 90) : GetSysColor(COLOR_BTNSHADOW);
    if (pressed) face = Theme::IsDark() ? RGB(44, 44, 44) : GetSysColor(COLOR_BTNSHADOW);
    if (disabled) face = Theme::Bg();

    HBRUSH br = CreateSolidBrush(face);
    FillRect(dis->hDC, &dis->rcItem, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    wchar_t text[128] = {};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? Theme::Dim() : Theme::Text());
    HGDIOBJ oldFont = SelectObject(dis->hDC, GetWindowFont(dis->hwndItem));
    RECT rc = dis->rcItem;
    if (pressed) OffsetRect(&rc, 1, 1);
    DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);

    if (focused) {
        RECT r = dis->rcItem;
        InflateRect(&r, -3, -3);
        DrawFocusRect(dis->hDC, &r);
    }
    return TRUE;
}
