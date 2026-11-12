#include "HashPage.h"
#include "HashJob.h"
#include "HashEngine.h"
#include "HashExt.h"   // g_hModule：对话框资源在 DLL 里，不能用宿主进程的模块句柄
#include "Lang.h"
#include "Theme.h"
#include "PathUtil.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <cwctype>
#include <cstdio>
#include <string>
#include <vector>

#define TIMER_EXPORT    0x1001
#define TIMER_CLIPBOARD 0x1002

// 计算在用户真正切到「哈希」页时自动开始（见 RequestAutoCompute）：
// 打开属性页仍停留在「常规」等标签时不会读盘，只有这一页被显示出来才启动。
// StartJob 只重置控件并起一个后台线程，读盘不发生在 UI 线程上。
//
// 多选时按选中顺序串行计算，一次读一个文件；当前文件的下标在 st->cur，
// 文件间切换通过「i/N ▾」下拉按钮（IDC_FILESEL）完成，切换只是重渲染，
// 不重读盘。

struct PageState {
    PageContext* ctx;
    bool done = false;           // 当前 Job 整体结束（全部文件算完 / 取消 / 出错）
    bool running = false;
    bool autoStarted = false;    // 自动计算只触发一次：用户取消后不再自作主张重开
    // 最新任务的序号。消息只带 Job*，而 Job 释放后新任务可能复用同一地址
    // （ABA 复用），单靠指针比对会把旧任务的完成消息误判成新任务的。
    // 因此每条消息额外带上产生它的 seq，与这里比对后才处理。
    UINT jobSeq = 0;
    size_t cur = 0;              // 多选时的当前文件下标（单文件恒为 0）
    int match = -1;              // -1 中性，-2 不匹配，>=0 匹配的算法下标
    std::wstring idleStatus;     // 导出提示结束后恢复的状态栏文字
    bool upper = false;          // 哈希值大写显示
    bool algo[kHashCount];       // 各算法是否勾选
    HBRUSH bgBrush = nullptr;
    HBRUSH panelBrush = nullptr;
    HFONT menuFont = nullptr;    // 自绘菜单项用系统菜单字体（首次打开菜单时创建）
    HFONT listFont = nullptr;    // 列表等宽字体（十六进制哈希对齐易读）
    HWND tip = nullptr;          // 工具提示控件
    bool cueShown = false;       // 校验框当前显示的是占位提示（不是用户输入）
};

// 前置声明：ListView 子类化过程要用到列头自绘（定义在「外观」一节）
static LRESULT CustomDrawHeader(LPNMCUSTOMDRAW cd);
static void RenderCurrent(HWND hwnd, PageState* st);
// 设置对话框（定义在「设置对话框」一节）也复用按钮自绘
static LRESULT DrawOwnerButton(const DRAWITEMSTRUCT* dis);
// 设置对话框里的「卸载…」按钮复用同一套卸载流程（定义在「卸载」一节）
static void DoUninstall(HWND hwnd);

// ---------------------------------------------------------------- 工具函数

static void SetStatus(HWND hwnd, const std::wstring& text) {
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
static void UpdateCopySelState(HWND hwnd, PageState* st) {
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

static std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

// 从 Job 里抄出当前文件（下标 idx）的结果快照。调用方必须已持 job 判空；
// 在 cs 内复制，出锁后再用，避免持锁调用 UI。
struct ResultSnap {
    std::wstring hex[kHashCount];
    FileChangeInfo change;
    bool finished = false;
    std::wstring error;          // kCancelledResult 表示已取消
    double elapsed = 0;
};

static void SnapCurrent(Job* job, size_t idx, ResultSnap* out) {
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
static std::wstring BuildAllLines(HWND hwnd, const wchar_t* fileName,
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

// ---------------------------------------------------------------- 设置持久化

// 设置存 HKCU\Software\HashPage：
//   Algos  DWORD 位掩码（bit0=CRC32 … bit4=SHA-512），缺省 0x1F 全选
//   Upper  DWORD 1=大写
// 与 LangId 同键，读取时机与语言一致（首次使用时，不在 DllMain 里碰注册表）。
static const wchar_t* kSettingsKey = L"Software\\HashPage";

static void LoadSettings(PageState* st) {
    for (int i = 0; i < kHashCount; ++i) st->algo[i] = true;
    st->upper = false;

    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    DWORD algos = 0, sz = sizeof(algos);
    if (RegQueryValueExW(h, L"Algos", 0, nullptr, reinterpret_cast<LPBYTE>(&algos), &sz) == ERROR_SUCCESS) {
        for (int i = 0; i < kHashCount; ++i)
            st->algo[i] = (algos & (1u << i)) != 0;
    }
    DWORD upper = 0;
    sz = sizeof(upper);
    if (RegQueryValueExW(h, L"Upper", 0, nullptr, reinterpret_cast<LPBYTE>(&upper), &sz) == ERROR_SUCCESS)
        st->upper = upper != 0;
    RegCloseKey(h);
}

static void SaveSettings(const PageState* st) {
    DWORD algos = 0;
    for (int i = 0; i < kHashCount; ++i)
        if (st->algo[i]) algos |= (1u << i);
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &h, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(h, L"Algos", 0, REG_DWORD,
                  reinterpret_cast<const BYTE*>(&algos), sizeof(algos));
    DWORD upper = st->upper ? 1u : 0u;
    RegSetValueExW(h, L"Upper", 0, REG_DWORD,
                  reinterpret_cast<const BYTE*>(&upper), sizeof(upper));
    RegCloseKey(h);
}

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
static void RebuildList(HWND hwnd, PageState* st, const wchar_t* placeholder) {
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

static void EvaluateVerify(HWND hwnd, PageState* st) {
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

static void RenderCurrent(HWND hwnd, PageState* st) {
    RebuildList(hwnd, st, nullptr);
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

// ---------------------------------------------------------------- 导出

// Vista+ 的 IFileSaveDialog：比 GetSaveFileNameW 更现代，且不需要 comdlg32 的
// OPENFILENAME 结构体，长路径与自定义位置支持更好。
static bool PickSavePath(HWND owner, const std::wstring& suggested, std::wstring* out) {
    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return false;

    bool ok = false;
    const COMDLG_FILTERSPEC filters[] = {
        { Lang::Get(Lang::Id::FilterSums), L"*.sums" },
        { Lang::Get(Lang::Id::FilterAll), L"*.*" },
    };
    dialog->SetFileTypes(2, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"sums");
    dialog->SetFileName(suggested.c_str());

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    }

    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return ok;
}

// 从 Job 的结果直接拼多文件导出内容（不经过列表控件，列表只显示当前文件）
static std::wstring BuildMultiLines(Job* job, const bool algo[kHashCount]) {
    std::wstring all;
    for (size_t f = 0; f < job->paths.size(); ++f) {
        if (f >= job->results.size() || !job->results[f].finished) continue;
        const wchar_t* name = PathFindFileNameW(job->paths[f].c_str());
        for (int i = 0; i < kHashCount; ++i) {
            if (!algo[i] || job->results[f].hex[i].empty()) continue;
            all += job->results[f].hex[i];
            all += L"  ";
            all += name;
            all += L"\r\n";
        }
    }
    return all;
}

static void DoExport(HWND hwnd, PageState* st) {
    if (!st->done) {
        SetStatus(hwnd, Lang::Get(Lang::Id::ComputeFirst));
        return;
    }
    Job* job = st->ctx->job;
    if (!job) return;

    const bool multi = job->paths.size() > 1;
    const wchar_t* name = PathFindFileNameW(job->paths[st->cur].c_str());
    // 多选时建议名用公共前缀更自然，这里取第一个文件的名词干 + _hashes
    std::wstring suggested;
    if (multi) {
        const wchar_t* dot = wcsrchr(name, L'.');
        std::wstring stem = dot ? std::wstring(name, dot - name) : std::wstring(name);
        suggested = stem + L"_hashes.sums";
    } else {
        suggested = std::wstring(name) + L".sums";
    }

    std::wstring target;
    if (!PickSavePath(hwnd, suggested, &target)) return;

    // 不允许把结果写回任何被哈希的文件：CREATE_ALWAYS 会直接截断源文件
    for (const auto& p : job->paths) {
        if (PathUtil::SameFile(target, p)) {
            SetStatus(hwnd, Lang::Get(Lang::Id::SourceIsTarget));
            return;
        }
    }

    std::string utf8;
    size_t exported = 0;
    if (multi) {
        EnterCriticalSection(&job->cs);
        const std::wstring all = BuildMultiLines(job, st->algo);
        size_t count = 0;
        for (size_t f = 0; f < job->results.size(); ++f)
            if (job->results[f].finished) ++count;
        LeaveCriticalSection(&job->cs);
        utf8 = ToUtf8(all);
        exported = count;
    } else {
        utf8 = ToUtf8(BuildAllLines(hwnd, name, st->algo));
        exported = 1;
    }

    HANDLE f = CreateFileW(LongPath(target).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        SetStatus(hwnd, Lang::Get(Lang::Id::ExportFail));
        return;
    }
    DWORD written = 0;
    const BOOL wrote = WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    // 落盘后再关：CloseHandle 不保证数据已写出，磁盘满时要靠 Flush 才能发现
    const BOOL flushed = wrote && written == utf8.size() ? FlushFileBuffers(f) : FALSE;
    CloseHandle(f);
    if (!flushed) {
        SetStatus(hwnd, Lang::Get(Lang::Id::ExportWriteFail));
        return;
    }

    if (multi) {
        wchar_t buf[128];
        swprintf(buf, 128, Lang::Get(Lang::Id::MultiExportHint),
                 static_cast<unsigned>(exported));
        SetStatus(hwnd, buf);
    } else {
        SetStatus(hwnd, Lang::Get(Lang::Id::Exported));
    }
    SetTimer(hwnd, TIMER_EXPORT, 2000, nullptr);
}

// ---------------------------------------------------------------- 弹出菜单

// 版本号（.rc 的 FILEVERSION 需同步改；菜单「关于」项显示的就是它）
static constexpr wchar_t kVersionText[] = L"1.3.0";

// 自绘菜单项：所有弹出菜单统一走 MF_OWNERDRAW。
// 浅色下模仿系统菜单配色；深色下套页面主题（系统菜单不跟随暗色，
// 直接用 TrackPopupMenu 会在深色页面上开出一块浅色菜单）。
struct MenuEntry {
    const wchar_t* text;   // nullptr = 分隔线
    UINT id;               // WM_COMMAND 命令 ID
    bool checked;
    bool underline;        // 文字下画线（表示可点击的外链）
};

// 菜单字体：首次使用时从系统菜单字体缓存到页面状态
static HFONT EnsureMenuFont(PageState* st) {
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
static void DrawMenuEntry(const DRAWITEMSTRUCT* dis, HFONT font) {
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
static void ShowFilesMenu(HWND hwnd, PageState* st) {
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
static void OpenProjectPage(HWND hwnd) {
    // ShellExecute 不需要可写命令行缓冲，也无 PATH_MAX 限制
    ShellExecuteW(hwnd, L"open",
                  L"https://github.com/louisgeek/LG_Hash",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- 设置对话框

// 独立设置对话框（参考 HashCheck 的形态）：算法勾选网格 + 显示选项 +
// 左下角项目链接，底部确定/取消。属性页不再内联这些控件，页面更清爽。
struct SettingsDlgState {
    bool algo[kHashCount];
    bool upper = false;
    bool dark = false;
    HBRUSH bg = nullptr;
    HFONT linkFont = nullptr;
};

static const int kSetAlgoIds[kHashCount] = {
    IDC_SET_CRC32, IDC_SET_MD5, IDC_SET_SHA1, IDC_SET_SHA256, IDC_SET_SHA512,
};
static const Lang::Id kSetAlgoLangs[kHashCount] = {
    Lang::Id::AlgoCRC32, Lang::Id::AlgoMD5, Lang::Id::AlgoSHA1,
    Lang::Id::AlgoSHA256, Lang::Id::AlgoSHA512,
};

static INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto ds = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        ds = reinterpret_cast<SettingsDlgState*>(lParam);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));
        if (!ds) return FALSE;

        Theme::Refresh();
        ds->dark = Theme::IsDark();
        if (ds->dark) ds->bg = CreateSolidBrush(Theme::Bg());

        SetWindowTextW(dlg, Lang::Get(Lang::Id::SettingsTitle));
        SetDlgItemTextW(dlg, IDC_SET_ALGOGROUP, Lang::Get(Lang::Id::AlgoGroup));
        SetDlgItemTextW(dlg, IDC_SET_DISPGROUP, Lang::Get(Lang::Id::DisplayGroup));
        SetDlgItemTextW(dlg, IDOK, Lang::Get(Lang::Id::DlgOk));
        SetDlgItemTextW(dlg, IDCANCEL, Lang::Get(Lang::Id::DlgCancel));
        SetDlgItemTextW(dlg, IDC_SET_UPPER, Lang::Get(Lang::Id::MenuUppercase));
        SetDlgItemTextW(dlg, IDC_SET_UNINSTALL, Lang::Get(Lang::Id::MenuUninstall));
        for (int i = 0; i < kHashCount; ++i) {
            SetDlgItemTextW(dlg, kSetAlgoIds[i], Lang::Get(kSetAlgoLangs[i]));
            SendMessageW(GetDlgItem(dlg, kSetAlgoIds[i]), BM_SETCHECK,
                         ds->algo[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        SendMessageW(GetDlgItem(dlg, IDC_SET_UPPER), BM_SETCHECK,
                     ds->upper ? BST_CHECKED : BST_UNCHECKED, 0);

        // 左下角项目链接：显示版本号，套下划线字体（静态文本不支持富文本）
        {
            wchar_t link[64];
            swprintf(link, 64, L"HashPage %s", kVersionText);
            HWND ctl = GetDlgItem(dlg, IDC_SET_LINK);
            SetWindowTextW(ctl, link);
            LOGFONTW lf{};
            HFONT base = reinterpret_cast<HFONT>(SendMessageW(dlg, WM_GETFONT, 0, 0));
            if (base && GetObjectW(base, sizeof(lf), &lf)) {
                lf.lfUnderline = TRUE;
                ds->linkFont = CreateFontIndirectW(&lf);
                if (ds->linkFont)
                    SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(ds->linkFont), TRUE);
            }
        }

        if (ds->dark) {
            // 深色下按钮同样走自绘，避免浅色按钮压在深色底上
            for (int id : { IDOK, IDCANCEL, IDC_SET_UNINSTALL }) {
                HWND b = GetDlgItem(dlg, id);
                if (!b) continue;
                const LONG_PTR style = GetWindowLongPtrW(b, GWL_STYLE);
                SetWindowLongPtrW(b, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
                InvalidateRect(b, nullptr, TRUE);
            }
        }
        return TRUE;
    }

    case WM_COMMAND:
        if (!ds) break;
        switch (LOWORD(wParam)) {
        case IDOK:
            for (int i = 0; i < kHashCount; ++i) {
                ds->algo[i] =
                    (SendMessageW(GetDlgItem(dlg, kSetAlgoIds[i]), BM_GETCHECK, 0, 0)
                     & BST_CHECKED) != 0;
            }
            ds->upper =
                (SendMessageW(GetDlgItem(dlg, IDC_SET_UPPER), BM_GETCHECK, 0, 0)
                 & BST_CHECKED) != 0;
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        case IDC_SET_LINK:
            if (HIWORD(wParam) == STN_CLICKED) OpenProjectPage(dlg);
            return TRUE;
        case IDC_SET_UNINSTALL:
            // 卸载交给安装器的独立进程；触发后属性页会随反注册关闭
            if (HIWORD(wParam) == BN_CLICKED) DoUninstall(dlg);
            return TRUE;
        default:
            break;
        }
        break;

    case WM_DRAWITEM:
        return DrawOwnerButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));

    case WM_CTLCOLORSTATIC:
        if (ds) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            SetBkMode(hdc, TRANSPARENT);
            if (id == IDC_SET_LINK) {
                // 链接用蓝色（深色下也是蓝，保证可辨识）
                SetTextColor(hdc, Theme::IsDark() ? RGB(106, 168, 255) : RGB(0, 102, 204));
            } else {
                SetTextColor(hdc, Theme::IsDark() ? Theme::Text() : GetSysColor(COLOR_WINDOWTEXT));
            }
            return reinterpret_cast<INT_PTR>(ds->dark ? ds->bg : GetSysColorBrush(COLOR_BTNFACE));
        }
        break;

    case WM_CTLCOLORBTN:
        if (ds && ds->dark) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, Theme::Text());
            return reinterpret_cast<INT_PTR>(ds->bg);
        }
        break;

    case WM_ERASEBKGND:
        if (ds && ds->dark) {
            RECT rc;
            GetClientRect(dlg, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, ds->bg);
            return 1;
        }
        break;

    case WM_DESTROY:
        if (ds) {
            if (ds->bg) DeleteObject(ds->bg);
            if (ds->linkFont) DeleteObject(ds->linkFont);
        }
        break;
    }
    return FALSE;
}

// 打开设置对话框；点「确定」时把选择写回并刷新属性页
static void OpenSettings(HWND hwnd, PageState* st) {
    SettingsDlgState ds{};
    for (int i = 0; i < kHashCount; ++i) ds.algo[i] = st->algo[i];
    ds.upper = st->upper;

    if (DialogBoxParamW(g_hModule, MAKEINTRESOURCEW(IDD_SETTINGS),
                        hwnd, SettingsProc, reinterpret_cast<LPARAM>(&ds)) != 1) {
        return;   // 取消：不写回
    }

    bool changed = ds.upper != st->upper;
    for (int i = 0; i < kHashCount; ++i) {
        if (ds.algo[i] != st->algo[i]) changed = true;
        st->algo[i] = ds.algo[i];
    }
    st->upper = ds.upper;
    if (!changed) return;

    SaveSettings(st);
    if (st->done) {
        RenderCurrent(hwnd, st);   // 勾选/大小写变化只重渲染，不重读盘
        EvaluateVerify(hwnd, st);
    }
}

// ---------------------------------------------------------------- 卸载

// 属性页里的「卸载」按钮：读注册表卸载条目里的命令，确认后启动安装器 /uninstall。
// 不在 explorer 进程内直接动手——反注册和删文件都交给独立进程，失败也不影响宿主。
static void DoUninstall(HWND hwnd) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HashPage",
                      0, KEY_READ, &h) != ERROR_SUCCESS) {
        MessageBoxW(hwnd, Lang::Get(Lang::Id::UninstallMissing),
                    Lang::Get(Lang::Id::PageTitle), MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t cmd[MAX_PATH * 2] = {};
    DWORD sz = sizeof(cmd);
    const LSTATUS got = RegQueryValueExW(h, L"UninstallString", 0, nullptr,
                                         reinterpret_cast<LPBYTE>(cmd), &sz);
    RegCloseKey(h);
    if (got != ERROR_SUCCESS || !cmd[0]) {
        MessageBoxW(hwnd, Lang::Get(Lang::Id::UninstallMissing),
                    Lang::Get(Lang::Id::PageTitle), MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (MessageBoxW(hwnd, Lang::Get(Lang::Id::UninstallConfirm),
                    Lang::Get(Lang::Id::PageTitle), MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2)
        != IDOK) {
        return;
    }

    // UninstallString 形如 "C:\...\HashPage.exe" /uninstall
    // 交给 CreateProcess 启动独立进程：反注册和删文件都不在 explorer 内发生
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmdline[MAX_PATH * 2];
    wcscpy_s(cmdline, cmd);
    // CreateProcess 的命令行必须可写
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(hwnd, Lang::Get(Lang::Id::UninstallFailed),
                    Lang::Get(Lang::Id::PageTitle), MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // 卸载器会反注册扩展；宿主属性窗口随即关闭（页面销毁走正常 WM_DESTROY 清理）
}

// ---------------------------------------------------------------- 外观

// 深色下列头也要自绘：header 是独立控件，ListView 的颜色设置管不到它
static LRESULT CustomDrawHeader(LPNMCUSTOMDRAW cd) {
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
static void SetVerifyCue(HWND hwnd, PageState* st, bool show) {
    HWND edit = GetDlgItem(hwnd, IDC_VERIFY);
    if (!edit) return;
    // 必须先置标志再改文本：SetWindowText 会同步触发 EN_CHANGE，
    // 若标志还是旧值，占位文字会被当成用户输入去校验，误显示「不匹配」
    st->cueShown = show;
    SetWindowTextW(edit, show ? Lang::Get(Lang::Id::VerifyCue) : L"");
}

static void ApplyLanguage(HWND hwnd) {
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
static void EnableOwnerDraw(HWND page) {
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
static LRESULT DrawOwnerButton(const DRAWITEMSTRUCT* dis) {
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

// ---------------------------------------------------------------- 主流程

INT_PTR CALLBACK HashPageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

UINT CALLBACK HashPageCallback(HWND, UINT msg, LPPROPSHEETPAGEW ppsp) {
    // 页面从未被打开时也会收到 RELEASE，上下文在此统一释放
    if (msg == PSPCB_RELEASE && ppsp)
        delete reinterpret_cast<PageContext*>(ppsp->lParam);
    return 1;
}


