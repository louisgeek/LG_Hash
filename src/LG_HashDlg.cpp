
// 属性页的两个「独立窗口」职责：设置对话框、导出文件对话框。
// 它们与主流程只通过 PageState 和少量已导出函数交互。

#include "LG_HashInternal.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>

#include "HashEngine.h"   // LongPath
#include "HashExt.h"      // g_hModule：对话框资源在 DLL 里，不能用宿主进程的模块句柄
#include "Lang.h"
#include "PathUtil.h"
#include "Theme.h"
#include "resource.h"
#include "version.h"


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

// ---------------------------------------------------------------- 设置持久化

// 设置存 HKCU\Software\LG_Hash：
//   Algos  DWORD 位掩码（bit0=CRC32 … bit4=SHA-512），缺省 0x1F 全选
//   Upper  DWORD 1=大写
// 与 LangId 同键，读取时机与语言一致（首次使用时，不在 DllMain 里碰注册表）。
static const wchar_t* kSettingsKey = L"Software\\LG_Hash";

void LoadSettings(PageState* st) {
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
        { Lang::Get(Lang::Id::FilterSums), L"*.SUMS.txt" },
        { Lang::Get(Lang::Id::FilterAll), L"*.*" },
    };
    dialog->SetFileTypes(2, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"SUMS.txt");
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

void DoExport(HWND hwnd, PageState* st) {
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
        suggested = stem + L"_hashes.SUMS.txt";
    } else {
        suggested = std::wstring(name) + L".SUMS.txt";
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

// ---------------------------------------------------------------- 设置对话框

// 版本号（唯一来源：src\version.h，改那里即可）
static constexpr wchar_t kVersionText[] = LG_HASH_VERSION_SHORT;

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

        Lang::Refresh();
        Theme::Refresh();
        ds->dark = Theme::IsDark();
        if (ds->dark) ds->bg = CreateSolidBrush(Theme::Bg());

        SetWindowTextW(dlg, Lang::Get(Lang::Id::SettingsTitle));
        SetDlgItemTextW(dlg, IDC_SET_ALGOGROUP, Lang::Get(Lang::Id::AlgoGroup));
        SetDlgItemTextW(dlg, IDC_SET_DISPGROUP, Lang::Get(Lang::Id::DisplayGroup));
        SetDlgItemTextW(dlg, IDOK, Lang::Get(Lang::Id::DlgOk));
        SetDlgItemTextW(dlg, IDCANCEL, Lang::Get(Lang::Id::DlgCancel));
        SetDlgItemTextW(dlg, IDC_SET_UPPER, Lang::Get(Lang::Id::MenuUppercase));
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
            swprintf(link, 64, L"LG_Hash v%s", kVersionText);
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
            for (int id : { IDOK, IDCANCEL }) {
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
void OpenSettings(HWND hwnd, PageState* st) {
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
