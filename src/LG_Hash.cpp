
// 属性页主流程：窗口消息分发、计算任务编排、校验、完成汇总。
// 设置对话框与导出见 LG_HashDlg.cpp，自绘与菜单见 LG_HashDraw.cpp。

#include "LG_HashInternal.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <cwctype>
#include <cstdio>
#include <string>
#include <vector>

#include "Lang.h"
#include "Theme.h"
#include "resource.h"


// ---------------------------------------------------------------- 工具函数

void SetStatus(HWND hwnd, const std::wstring& text) {
    SetDlgItemTextW(hwnd, IDC_STATUS, text.c_str());
}

static bool CopyText(HWND hwnd, const std::wstring& text) {
    // 剪贴板是全局独占资源，被其它程序占用是常态，重试几个周期再放弃
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(hwnd)) {
            const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (!mem) {
                CloseClipboard();
                return false;
            }
            void* p = GlobalLock(mem);
            if (!p) {
                GlobalFree(mem);
                CloseClipboard();
                return false;
            }
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);

            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, mem)) {
                CloseClipboard();
                return true;
            }
            GlobalFree(mem);
            CloseClipboard();
            return false;
        }
        Sleep(20);
    }
    return false;
}

// 复制成功后状态栏短暂显示提示，2 秒后由 TIMER_CLIPBOARD 恢复空闲状态
static void ShowTransientStatus(HWND hwnd, Lang::Id id) {
    SetStatus(hwnd, Lang::Get(id));
    SetTimer(hwnd, TIMER_CLIPBOARD, 2000, nullptr);
}

// 「复制选中」只在列表有选中行且计算完成时可用
void UpdateCopySelState(HWND hwnd, PageState* st) {
    HWND list = GetDlgItem(hwnd, IDC_HASHLIST);
    const bool selected = st->done &&
                          ListView_GetNextItem(list, -1, LVNI_SELECTED) >= 0;
    EnableWindow(GetDlgItem(hwnd, IDC_COPYSEL), selected);
}

// 复制列表当前选中的那一行；无选中则什么都不做。
// 点按钮、双击行、Ctrl+C 共用这一条路径，避免三处逻辑漂移。
static bool CopySelectedRow(HWND hwnd, PageState* st) {
    if (!st->done) return false;
    HWND list = GetDlgItem(hwnd, IDC_HASHLIST);
    const int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel < 0) return false;
    wchar_t buf[256] = {};
    ListView_GetItemText(list, sel, 1, buf, 256);
    if (CopyText(hwnd, buf)) {
        ShowTransientStatus(hwnd, Lang::Id::Copied);
        return true;
    }
    SetStatus(hwnd, Lang::Get(Lang::Id::ClipboardBusy));
    SetTimer(hwnd, TIMER_CLIPBOARD, 2500, nullptr);
    return false;
}

static void FormatSize(ULONGLONG bytes, wchar_t* out, size_t cch) {
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    if (u == 0)
        swprintf(out, cch, Lang::Get(Lang::Id::Bytes), bytes);
    else
        swprintf(out, cch, Lang::Get(Lang::Id::SizeWithUnit), v, units[u], bytes);
}

// 从 Job 里抄出当前文件（下标 idx）的结果快照（定义见 LG_HashInternal.h）
void SnapCurrent(Job* job, size_t idx, ResultSnap* out) {
    EnterCriticalSection(&job->cs);
    if (idx < job->results.size()) {
        for (int i = 0; i < kHashCount; ++i) out->hex[i] = job->results[idx].hex[i];
        out->change = job->results[idx].change;
        out->finished = job->results[idx].finished;
        out->error = job->results[idx].error;
    }
    out->elapsed = job->elapsedSec;
    LeaveCriticalSection(&job->cs);
}

// 按 "哈希  文件名" 的格式拼出全部已勾选算法的结果（单文件路径用）。
std::wstring BuildAllLines(HWND hwnd, const wchar_t* fileName,
                                  const bool algo[kHashCount]) {
    HWND list = GetDlgItem(hwnd, IDC_HASHLIST);
    std::wstring all;
    for (int i = 0; i < kHashCount; ++i) {
        if (!algo[i]) continue;
        wchar_t buf[256] = {};
        ListView_GetItemText(list, i, 1, buf, 256);
        if (wcslen(buf) == 0 || wcscmp(buf, L"—") == 0) continue;
        all += buf;
        all += L"  ";
        all += fileName;
        all += L"\r\n";
    }
    return all;
}

// ---------------------------------------------------------------- 计算任务

static void StartJob(HWND hwnd, PageState* st) {
    // 一个算法都没勾时读盘没有意义，提示后直接返回
    bool any = false;
    for (int i = 0; i < kHashCount; ++i) any = any || st->algo[i];
    if (!any) {
        SetStatus(hwnd, Lang::Get(Lang::Id::NoAlgoSelected));
        return;
    }

    StopJob(st->ctx->job);
    st->ctx->job = nullptr;

    // seq 单调递增，恰好到 0xFFFF 时跳回 1（0 表示未分配），避免与旧值回绕相等
    if (++st->jobSeq > 0xFFFF) st->jobSeq = 1;

    Job* job = CreateJob(hwnd, st->jobSeq, st->ctx->paths, st->algo);
    if (!job) {
        SetStatus(hwnd, Lang::Get(Lang::Id::ErrOutOfMemory));
        return;
    }
    st->ctx->job = job;
    st->done = false;
    st->running = true;
    st->match = -1;
    st->cur = 0;
    st->idleStatus.clear();

    RebuildList(hwnd, st, L"…");
    SetStatus(hwnd, Lang::Get(Lang::Id::Computing));
    SetDlgItemTextW(hwnd, IDC_MATCHRESULT, L"");
    SetDlgItemTextW(hwnd, IDC_REHASH, Lang::Get(Lang::Id::Cancel));
    EnableWindow(GetDlgItem(hwnd, IDC_COPYSEL), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_COPYALL), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_EXPORT), FALSE);

    HWND progress = GetDlgItem(hwnd, IDC_PROGRESS);
    ShowWindow(progress, SW_SHOW);
    SendMessageW(progress, PBM_SETRANGE32, 0, 100);
    SendMessageW(progress, PBM_SETPOS, 0, 0);

    if (!StartWorker(job)) {
        StopJob(st->ctx->job);
        st->ctx->job = nullptr;
        st->running = false;
        SetStatus(hwnd, Lang::Get(Lang::Id::CannotThread));
        SetDlgItemTextW(hwnd, IDC_REHASH, Lang::Get(Lang::Id::Recompute));
        return;
    }
}

// ---------------------------------------------------------------- 自动计算

// 页面第一次被切到前台时自动开始计算，省掉“切过来还要再点一下”的一步。
// PSN_SETACTIVE 只在页面真正显示出来时才到——打开属性窗口停在「常规」页不会读盘。
// StartJob 只做控件重置并起一个后台线程，不在这里读文件，切换动画不受影响。
static void RequestAutoCompute(HWND hwnd, PageState* st) {
    if (!st || st->autoStarted) return;
    if (st->done || st->running) return;
    st->autoStarted = true;
    StartJob(hwnd, st);
}

// ---------------------------------------------------------------- 校验

void EvaluateVerify(HWND hwnd, PageState* st) {
    // 占位提示不是用户输入：空态直接按「未填写」处理，不显示任何匹配结果
    wchar_t buf[512] = {};
    if (!st->cueShown) GetDlgItemTextW(hwnd, IDC_VERIFY, buf, 512);
    std::wstring input;
    for (const wchar_t* p = buf; *p; ++p)
        if (!iswspace(static_cast<wint_t>(*p))) input += *p;

    if (input.empty()) {
        st->match = -1;
        SetDlgItemTextW(hwnd, IDC_MATCHRESULT, L"");
    } else if (!st->done) {
        st->match = -1;
        SetDlgItemTextW(hwnd, IDC_MATCHRESULT,
                        st->running ? Lang::Get(Lang::Id::Waiting) : Lang::Get(Lang::Id::NotComputed));
    } else {
        int found = -1;
        Job* job = st->ctx->job;
        if (job && st->cur < job->paths.size()) {
            ResultSnap snap;
            SnapCurrent(job, st->cur, &snap);
            if (snap.finished) {
                for (int i = 0; i < kHashCount; ++i) {
                    if (!st->algo[i]) continue;   // 未勾选的算法不参与匹配
                    if (_wcsicmp(input.c_str(), snap.hex[i].c_str()) == 0) { found = i; break; }
                }
            }
        }
        st->match = found >= 0 ? found : -2;
        if (found >= 0)
            swprintf(buf, 512, Lang::Get(Lang::Id::Match), kHashAlgos[found].name);
        else
            wcscpy_s(buf, Lang::Get(Lang::Id::NoMatch));
        SetDlgItemTextW(hwnd, IDC_MATCHRESULT, buf);
    }
    InvalidateRect(GetDlgItem(hwnd, IDC_MATCHRESULT), nullptr, TRUE);
}

// ---------------------------------------------------------------- 完成

static void ShowChangeWarning(HWND hwnd, const FileChangeInfo& change) {
    if (!change.valid) {
        SetDlgItemTextW(hwnd, IDC_CHANGEWARN, Lang::Get(Lang::Id::ChangeUnknown));
        ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_SHOW);
        return;
    }
    if (!change.changed) {
        ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);
        return;
    }
    wchar_t buf[256];
    if (change.SizeChanged()) {
        swprintf(buf, 256, Lang::Get(Lang::Id::ChangedSize),
                 change.sizeBefore, change.sizeAfter);
    } else {
        wcscpy_s(buf, sizeof(buf) / sizeof(buf[0]), Lang::Get(Lang::Id::Changed));
    }
    SetDlgItemTextW(hwnd, IDC_CHANGEWARN, buf);
    ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_SHOW);
    InvalidateRect(GetDlgItem(hwnd, IDC_CHANGEWARN), nullptr, TRUE);
}

// 刷新「文件 i/N」信息行与上/下导航按钮的可用态
// 多选时用「◀ i/N ▾ ▶」三件套切换文件：
// 中间的按钮点击列出全部文件，两侧箭头快速前后切换；单文件时全部隐藏。
static void UpdateFileInfo(HWND hwnd, PageState* st) {
    const size_t n = st->ctx->paths.size();
    const std::wstring& path = st->ctx->paths[st->cur];
    const wchar_t* name = PathFindFileNameW(path.c_str());

    wchar_t info[600];
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        wchar_t sizeText[96];
        FormatSize(sz, sizeText, 96);
        swprintf(info, 600, L"%s — %s", name, sizeText);
    } else {
        wcscpy_s(info, name);
    }
    SetDlgItemTextW(hwnd, IDC_FILEINFO, info);

    // 多选时用「◀ i/N ▾ ▶」三件套切换：中间下拉列出全部文件，
    // 两侧箭头快速前后切换；单文件时三个控件都隐藏。
    HWND sel = GetDlgItem(hwnd, IDC_FILESEL);
    HWND prev = GetDlgItem(hwnd, IDC_PREV);
    HWND next = GetDlgItem(hwnd, IDC_NEXT);
    if (n > 1) {
        wchar_t btn[32];
        swprintf(btn, 32, L"%u/%u ▾",
                 static_cast<unsigned>(st->cur + 1), static_cast<unsigned>(n));
        SetWindowTextW(sel, btn);
        ShowWindow(sel, SW_SHOW);
        ShowWindow(prev, SW_SHOW);
        ShowWindow(next, SW_SHOW);
        EnableWindow(prev, st->cur > 0);
        EnableWindow(next, st->cur + 1 < n);
    } else {
        ShowWindow(sel, SW_HIDE);
        ShowWindow(prev, SW_HIDE);
        ShowWindow(next, SW_HIDE);
    }
}

// 切换当前显示的文件（多选）：只重渲染，不重读盘
static void SwitchFile(HWND hwnd, PageState* st, size_t idx) {
    if (idx >= st->ctx->paths.size() || idx == st->cur) return;
    st->cur = idx;
    UpdateFileInfo(hwnd, st);
    RenderCurrent(hwnd, st);

    // 状态栏与变更提示跟随当前文件
    Job* job = st->ctx->job;
    if (st->done && job && idx < job->paths.size()) {
        ResultSnap snap;
        SnapCurrent(job, idx, &snap);
        if (snap.finished) {
            wchar_t buf[96];
            swprintf(buf, 96, Lang::Get(Lang::Id::AllDone), snap.elapsed);
            SetStatus(hwnd, buf);
            st->idleStatus = buf;
            ShowChangeWarning(hwnd, snap.change);
        } else if (snap.error == kCancelledResult) {
            SetStatus(hwnd, Lang::Get(Lang::Id::Cancelled));
            ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);
        } else {
            SetStatus(hwnd, std::wstring(Lang::Get(Lang::Id::Failed)) + snap.error);
            ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);
        }
        EvaluateVerify(hwnd, st);
    }
}

static void OnDone(HWND hwnd, PageState* st, Job* job) {
    if (!job) return;

    EnterCriticalSection(&job->cs);
    const double sec = job->elapsedSec;
    const size_t n = job->paths.size();
    std::vector<FileResult> results = job->results;
    LeaveCriticalSection(&job->cs);

    st->running = false;
    ShowWindow(GetDlgItem(hwnd, IDC_PROGRESS), SW_HIDE);
    SetDlgItemTextW(hwnd, IDC_REHASH, Lang::Get(Lang::Id::Recompute));

    // 统计：成功多少、失败多少、是否整体取消
    size_t okCount = 0;
    bool cancelled = false;
    std::wstring firstError;
    size_t firstErrorIdx = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].finished) {
            ++okCount;
        } else if (results[i].error == kCancelledResult) {
            cancelled = true;
        } else if (results[i].error.size() &&
                   results[i].error != kCancelledResult && firstError.empty()) {
            firstError = results[i].error;
            firstErrorIdx = i;
        }
    }
    st->done = true;

    if (okCount == n && !cancelled) {
        // 全部成功（单文件路径与旧的 Done 文案一致）
        RenderCurrent(hwnd, st);
        wchar_t buf[96];
        swprintf(buf, 96, Lang::Get(n > 1 ? Lang::Id::AllDone : Lang::Id::Done), sec);
        SetStatus(hwnd, buf);
        st->idleStatus = buf;
        EnableWindow(GetDlgItem(hwnd, IDC_COPYSEL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_COPYALL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_EXPORT), TRUE);
        ResultSnap snap;
        SnapCurrent(job, st->cur, &snap);
        ShowChangeWarning(hwnd, snap.change);
        EvaluateVerify(hwnd, st);
        UpdateCopySelState(hwnd, st);   // 有选中行时才启用「复制选中」
    } else if (okCount > 0) {
        // 部分成功：定位到第一个失败的文件并提示
        RenderCurrent(hwnd, st);
        const wchar_t* name = PathFindFileNameW(job->paths[firstErrorIdx].c_str());
        wchar_t buf[512];
        swprintf(buf, 512, Lang::Get(Lang::Id::FileFailedOf),
                 static_cast<unsigned>(firstErrorIdx + 1), static_cast<unsigned>(n), name);
        SetStatus(hwnd, std::wstring(buf) + firstError);
        st->idleStatus.clear();
        EnableWindow(GetDlgItem(hwnd, IDC_COPYSEL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_COPYALL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_EXPORT), TRUE);
        ResultSnap snap;
        SnapCurrent(job, st->cur, &snap);
        if (snap.finished) ShowChangeWarning(hwnd, snap.change);
        else ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);
        EvaluateVerify(hwnd, st);
        UpdateCopySelState(hwnd, st);
    } else if (cancelled) {
        SetStatus(hwnd, Lang::Get(Lang::Id::Cancelled));
        RebuildList(hwnd, st, L"—");
        ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);
    } else {
        SetStatus(hwnd, std::wstring(Lang::Get(Lang::Id::Failed)) + firstError);
        RebuildList(hwnd, st, L"—");
        ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);
    }
}

// 列头的 NM_CUSTOMDRAW 发给父窗口（即 ListView），不会到达属性页对话框，
// 因此子类化 ListView 把它拦下来转发给 CustomDrawHeader。
static LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
    if (msg == WM_NOTIFY) {
        auto nm = reinterpret_cast<LPNMHDR>(lParam);
        HWND header = ListView_GetHeader(hwnd);
        if (nm && header && nm->hwndFrom == header && nm->code == NM_CUSTOMDRAW) {
            return CustomDrawHeader(reinterpret_cast<LPNMCUSTOMDRAW>(lParam));
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ListViewSubclassProc, 1);
    }
    (void)refData;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- 主流程

INT_PTR CALLBACK LG_HashProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto st = reinterpret_cast<PageState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        auto psp = reinterpret_cast<LPPROPSHEETPAGEW>(lParam);
        auto ctx = psp ? reinterpret_cast<PageContext*>(psp->lParam) : nullptr;
        if (!ctx || ctx->paths.empty()) return FALSE;

        st = new (std::nothrow) PageState{};
        if (!st) return FALSE;
        st->ctx = ctx;
        st->match = -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);
        // 两个都是「读注册表 + 缓存」的模块：每次建页都刷新，使「改完设置下次打开」
        // 即生效，不必重启 explorer.exe。值没变时 Lang::Refresh 只做一次轻量读。
        Lang::Refresh();
        Theme::Refresh();
        st->bgBrush = CreateSolidBrush(Theme::Bg());
        st->panelBrush = CreateSolidBrush(Theme::Panel());
        ApplyLanguage(hwnd);
        LoadSettings(st);
        SetVerifyCue(hwnd, st, true);   // 校验框初始显示占位提示
        if (Theme::IsDark()) EnableOwnerDraw(hwnd);

        // 算法勾选与大写显示已移入独立设置对话框（见 OpenSettings）：
        // 属性页只保留结果展示与校验，st->algo / st->upper 由 LoadSettings 加载

        UpdateFileInfo(hwnd, st);

        HWND list = GetDlgItem(hwnd, IDC_HASHLIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        // 十六进制哈希用等宽字体：字符对齐，逐位比对更轻松
        {
            NONCLIENTMETRICSW ncm{};
            ncm.cbSize = sizeof(ncm);
            LOGFONTW lf{};
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
                lf = ncm.lfMessageFont;
                wcscpy_s(lf.lfFaceName, L"Consolas");
                lf.lfWeight = FW_NORMAL;
                st->listFont = CreateFontIndirectW(&lf);
                if (st->listFont)
                    SendMessageW(list, WM_SETFONT,
                                 reinterpret_cast<WPARAM>(st->listFont), TRUE);
            }
        }
        // 悬停提示：文件跳转与「复制选中」的含义不比按钮文字更明显
        {
            st->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            auto addTip = [&](int id, Lang::Id lid) {
                HWND ctl = GetDlgItem(hwnd, id);
                if (!st->tip || !ctl) return;
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd = hwnd;
                ti.uId = reinterpret_cast<UINT_PTR>(ctl);
                ti.lpszText = const_cast<wchar_t*>(Lang::Get(lid));
                SendMessageW(st->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            };
            addTip(IDC_FILESEL, Lang::Id::FileSelectTip);
            addTip(IDC_COPYSEL, Lang::Id::CopySelTip);
        }
        if (Theme::IsDark()) {
            ListView_SetBkColor(list, Theme::Panel());
            ListView_SetTextBkColor(list, Theme::Panel());
            ListView_SetTextColor(list, Theme::Text());
            // 列头是 ListView 的子控件，需子类化 ListView 才能接管其自绘
            SetWindowSubclass(list, ListViewSubclassProc, 1, 0);
        }

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(Lang::Get(Lang::Id::AlgoColumn));
        col.cx = 90;
        col.iSubItem = 0;
        ListView_InsertColumn(list, 0, &col);

        // 哈希列按最长 SHA-512 定宽，超出部分横向滚动
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
        HDC dc = GetDC(list);
        HGDIOBJ old = SelectObject(dc, font);
        SIZE s{};
        GetTextExtentPoint32W(dc, L"0", 1, &s);
        SelectObject(dc, old);
        ReleaseDC(list, dc);
        col.pszText = const_cast<wchar_t*>(Lang::Get(Lang::Id::HashColumn));
        col.cx = s.cx * 128 + 16;
        col.iSubItem = 1;
        ListView_InsertColumn(list, 1, &col);

        // 初始行 = 当前勾选的算法（未勾选的不出现在列表里）
        RebuildList(hwnd, st, L"—");
        ShowWindow(GetDlgItem(hwnd, IDC_PROGRESS), SW_HIDE);
        ShowWindow(GetDlgItem(hwnd, IDC_CHANGEWARN), SW_HIDE);

        // 这一瞬的文字只在自动计算启动前可见（PSN_SETACTIVE 一到就切成「正在计算…」）
        SetStatus(hwnd, Lang::Get(Lang::Id::NotComputed));
        SetDlgItemTextW(hwnd, IDC_REHASH, Lang::Get(Lang::Id::Compute));
        return TRUE;
    }

    case WM_APP_PROGRESS: {
        // 双重校验：指针相同不代表是同一个任务（可能复用了地址），再比对序号。
        // 旧的进度消息若不丢弃，会把进度条拉回旧值。
        Job* job = st ? st->ctx->job : nullptr;
        if (!st || !st->running || !job) return 0;
        if (job != reinterpret_cast<Job*>(lParam)) return 0;
        const UINT seq = static_cast<UINT>(wParam >> 32);
        if (seq != job->seq) return 0;
        const unsigned fileIdx = static_cast<unsigned>((wParam >> 16) & 0xFFFF);
        const int pct = static_cast<int>(wParam & 0xFFFF);
        if (pct > 100) return 0;

        SendDlgItemMessageW(hwnd, IDC_PROGRESS, PBM_SETPOS, pct, 0);
        wchar_t buf[96];
        if (job->paths.size() > 1) {
            swprintf(buf, 96, L"%s %d%%",
                     Lang::Get(Lang::Id::Computing), pct);
            wchar_t head[160];
            swprintf(head, 160, Lang::Get(Lang::Id::FileOf),
                     fileIdx + 1, static_cast<unsigned>(job->paths.size()),
                     PathFindFileNameW(job->paths[fileIdx < job->paths.size() ? fileIdx : 0].c_str()));
            SetStatus(hwnd, std::wstring(head) + L" — " + buf);
        } else {
            swprintf(buf, 96, L"%s %d%%", Lang::Get(Lang::Id::Computing), pct);
            SetStatus(hwnd, buf);
        }
        return 0;
    }

    case WM_APP_DONE:
        // 快速连点「重新计算」时，旧线程的完成消息可能后到；Job 释放后新任务还可能
        // 复用同一地址，因此除指针外还要比对序号，否则会读到新任务的中间状态。
        if (st && st->ctx->job == reinterpret_cast<Job*>(lParam) &&
            static_cast<UINT>(wParam) == st->ctx->job->seq) {
            OnDone(hwnd, st, reinterpret_cast<Job*>(lParam));
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_EXPORT) {
            KillTimer(hwnd, TIMER_EXPORT);
            if (st && st->done && !st->idleStatus.empty()) SetStatus(hwnd, st->idleStatus);
        } else if (wParam == TIMER_CLIPBOARD) {
            KillTimer(hwnd, TIMER_CLIPBOARD);
            if (st) {
                // 恢复到空闲状态；尚未计算过时退回「未计算」提示
                if (!st->idleStatus.empty()) SetStatus(hwnd, st->idleStatus);
                else SetStatus(hwnd, Lang::Get(Lang::Id::NotComputed));
            }
        }
        return 0;

    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wParam)) {
        case IDC_REHASH:
            if (st->running) {
                if (st->ctx->job) SetEvent(st->ctx->job->cancel);
            } else {
                StartJob(hwnd, st);
            }
            break;

        case IDC_PREV:
            // 两侧箭头：快速前后切换（边界检查在 SwitchFile 里）
            if (HIWORD(wParam) == BN_CLICKED && st->cur > 0)
                SwitchFile(hwnd, st, st->cur - 1);
            break;

        case IDC_NEXT:
            if (HIWORD(wParam) == BN_CLICKED && st->cur + 1 < st->ctx->paths.size())
                SwitchFile(hwnd, st, st->cur + 1);
            break;

        case IDC_FILESEL:
            if (HIWORD(wParam) == BN_CLICKED) ShowFilesMenu(hwnd, st);
            break;

        case IDC_COPYSEL:
            CopySelectedRow(hwnd, st);
            break;

        case IDC_COPYALL:
            if (!st->done) break;
            if (!CopyText(hwnd, BuildAllLines(hwnd,
                          PathFindFileNameW(st->ctx->paths[st->cur].c_str()),
                          st->algo))) {
                SetStatus(hwnd, Lang::Get(Lang::Id::ClipboardBusy));
                SetTimer(hwnd, TIMER_CLIPBOARD, 2500, nullptr);
            } else {
                ShowTransientStatus(hwnd, Lang::Id::Copied);
            }
            break;

        case IDC_VERIFY:
            if (HIWORD(wParam) == EN_SETFOCUS) {
                // 聚焦时清掉占位提示，用户输入不被污染
                if (st->cueShown) SetVerifyCue(hwnd, st, false);
            } else if (HIWORD(wParam) == EN_KILLFOCUS) {
                // 失焦且仍为空时恢复占位提示
                wchar_t cur[512] = {};
                GetDlgItemTextW(hwnd, IDC_VERIFY, cur, 512);
                if (!st->cueShown && cur[0] == 0) SetVerifyCue(hwnd, st, true);
            } else if (HIWORD(wParam) == EN_CHANGE) {
                // 占位提示不参与校验：空态时清空结果，不显示「不匹配」
                if (st->cueShown) {
                    wchar_t cur[512] = {};
                    GetDlgItemTextW(hwnd, IDC_VERIFY, cur, 512);
                    if (wcscmp(cur, Lang::Get(Lang::Id::VerifyCue)) != 0) st->cueShown = false;
                }
                if (!st->cueShown) {
                    wchar_t cur[512] = {};
                    GetDlgItemTextW(hwnd, IDC_VERIFY, cur, 512);
                    if (cur[0] == 0) {
                        // 输入被清空：只清除结果，不判定为「不匹配」
                        st->match = -1;
                        SetDlgItemTextW(hwnd, IDC_MATCHRESULT, L"");
                    } else {
                        EvaluateVerify(hwnd, st);
                    }
                }
            }
            break;

        case IDC_EXPORT:
            DoExport(hwnd, st);
            break;

        case IDC_SETTINGS_BTN:
            if (HIWORD(wParam) == BN_CLICKED) OpenSettings(hwnd, st);
            break;

        default:
            // 文件跳转菜单项：IDM_FILE_BASE + 下标
            if (LOWORD(wParam) >= IDM_FILE_BASE &&
                LOWORD(wParam) < IDM_FILE_BASE + IDM_FILE_MAX) {
                SwitchFile(hwnd, st, static_cast<size_t>(LOWORD(wParam) - IDM_FILE_BASE));
            }
            break;
        }
        break;

    case WM_NOTIFY: {
        auto nm = reinterpret_cast<LPNMHDR>(lParam);
        // 属性表页面被切到前台：PSN_SETACTIVE 的 idFrom 为 0，必须先于列判断处理。
        // 用户从「常规」切到「哈希」时就自动开始算，不必再点一次按钮。
        if (nm && nm->code == PSN_SETACTIVE && st) {
            RequestAutoCompute(hwnd, st);
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (nm->code == NM_CUSTOMDRAW && Theme::IsDark()) {
            // 列头来自 ListView 的 Header 子控件，idFrom 可能是 0
            if (nm->hwndFrom == ListView_GetHeader(GetDlgItem(hwnd, IDC_HASHLIST)))
                return CustomDrawHeader(reinterpret_cast<LPNMCUSTOMDRAW>(lParam));
        }
        if (nm->idFrom == IDC_HASHLIST && st) {
            if (nm->code == NM_DBLCLK) {
                CopySelectedRow(hwnd, st);
            } else if (nm->code == LVN_ITEMCHANGED) {
                // 选中/取消选中会改变「复制选中」的可用性
                UpdateCopySelState(hwnd, st);
            } else if (nm->code == LVN_KEYDOWN) {
                // 列表上按 Ctrl+C 复制选中行，与系统习惯一致
                auto kd = reinterpret_cast<LPNMLVKEYDOWN>(lParam);
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (kd->wVKey == 'C' && ctrl) CopySelectedRow(hwnd, st);
            }
        }
        break;
    }

    case WM_MEASUREITEM: {
        // 自绘菜单项的尺寸按系统菜单字体算
        auto mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis && mis->CtlType == ODT_MENU && mis->itemData) {
            auto e = reinterpret_cast<const MenuEntry*>(mis->itemData);
            HFONT font = st ? EnsureMenuFont(st) : nullptr;
            if (!e->text) {   // 分隔线
                mis->itemWidth = 0;
                mis->itemHeight = 8;
                return TRUE;
            }
            HDC dc = GetDC(hwnd);
            HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
            SIZE sz{};
            GetTextExtentPoint32W(dc, e->text, static_cast<int>(wcslen(e->text)), &sz);
            if (old) SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
            mis->itemWidth = static_cast<UINT>(sz.cx) + 40;   // 勾选列 24 + 右留白 16
            mis->itemHeight = static_cast<UINT>(sz.cy + 10 > 24 ? sz.cy + 10 : 24);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;
        if (dis->CtlType == ODT_MENU) {
            DrawMenuEntry(dis, st ? st->menuFont : nullptr);
            return TRUE;
        }
        // 只有切换成 owner-draw 的按钮才会走到这里（见 EnableOwnerDraw）
        return DrawOwnerButton(dis);
    }

    case WM_CTLCOLORSTATIC: {
        if (!st) break;
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(ctl);
        SetBkMode(hdc, TRANSPARENT);

        if (id == IDC_MATCHRESULT) {
            if (st->match >= 0) SetTextColor(hdc, Theme::IsDark() ? RGB(106, 209, 122) : RGB(0, 128, 0));
            else if (st->match == -2) SetTextColor(hdc, Theme::IsDark() ? RGB(255, 120, 120) : RGB(192, 0, 0));
            else SetTextColor(hdc, Theme::Text());
            return reinterpret_cast<INT_PTR>(st->bgBrush);
        }
        if (id == IDC_CHANGEWARN) {
            SetTextColor(hdc, Theme::IsDark() ? RGB(255, 190, 92) : RGB(150, 90, 0));
            return reinterpret_cast<INT_PTR>(st->bgBrush);
        }
        if (Theme::IsDark()) {
            SetTextColor(hdc, Theme::Text());
            return reinterpret_cast<INT_PTR>(st->bgBrush);
        }
        break;
    }

    case WM_CTLCOLORBTN:
        if (Theme::IsDark() && st) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, Theme::Text());
            return reinterpret_cast<INT_PTR>(st->bgBrush);
        }
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        // 占位提示用次要文字色，和真实输入区分开
        if (st && st->cueShown && GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_VERIFY) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, Theme::IsDark() ? Theme::Panel() : GetSysColor(COLOR_WINDOW));
            SetTextColor(hdc, Theme::Dim());
            return reinterpret_cast<INT_PTR>(Theme::IsDark() ? st->panelBrush
                                                             : GetSysColorBrush(COLOR_WINDOW));
        }
        if (Theme::IsDark() && st) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, Theme::Panel());
            SetTextColor(hdc, Theme::Text());
            return reinterpret_cast<INT_PTR>(st->panelBrush);
        }
        break;

    case WM_ERASEBKGND:
        if (st && Theme::IsDark()) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, st->bgBrush);
            return 1;
        }
        break;

    case WM_PAINT:
        if (st && Theme::IsDark()) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            FillRect(dc, &ps.rcPaint, st->bgBrush);
            EndPaint(hwnd, &ps);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (st) {
            KillTimer(hwnd, TIMER_EXPORT);
            KillTimer(hwnd, TIMER_CLIPBOARD);
            StopJob(st->ctx->job);
            st->ctx->job = nullptr;
            if (st->bgBrush) DeleteObject(st->bgBrush);
            if (st->panelBrush) DeleteObject(st->panelBrush);
            if (st->menuFont) DeleteObject(st->menuFont);
            if (st->listFont) DeleteObject(st->listFont);
            if (st->tip) DestroyWindow(st->tip);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return FALSE;
}

UINT CALLBACK LG_HashCallback(HWND, UINT msg, LPPROPSHEETPAGEW ppsp) {
    // 页面从未被打开时也会收到 RELEASE，上下文在此统一释放
    if (msg == PSPCB_RELEASE && ppsp)
        delete reinterpret_cast<PageContext*>(ppsp->lParam);
    return 1;
}
