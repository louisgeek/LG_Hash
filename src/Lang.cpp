#include "Lang.h"

#include <atomic>

// 个别 SDK 版本未定义 LANGIDFROMLCID，这里兜底（低 16 位即 LANGID）
#ifndef LANGIDFROMLCID
#define LANGIDFROMLCID(lcid) ((WORD)((DWORD)(lcid) & 0xFFFF))
#endif

// Shell 扩展可能被多个线程同时调用 AddPages/Get，初始化必须是线程安全的。
// 用 acquire/release 的原子标志做一次性初始化，避免 std::call_once 在 DLL 卸载时的析构顺序问题。
static std::atomic<bool> g_chinese{true};
static std::atomic<bool> g_initialized{false};

// 上次读取到的原始 LangId 值（0 表示未设置，走系统默认）。Refresh() 用它判断
// 设置是否真的变了，避免每次打开属性页都无谓地重算。
static std::atomic<DWORD> g_rawLangId{0};

struct Entry { Lang::Id id; const wchar_t* zh; const wchar_t* en; };
static const Entry kTable[] = {
    { Lang::Id::PageTitle,      L"哈希",         L"Hash" },
    { Lang::Id::VerifyGroup,    L"校验",         L"Verify" },
    { Lang::Id::Computing,      L"正在计算…",     L"Computing…" },
    { Lang::Id::Done,           L"完成，用时 %.1f 秒", L"Done in %.1f s" },
    { Lang::Id::Cancelled,      L"已取消",        L"Cancelled" },
    { Lang::Id::Failed,         L"计算失败：",    L"Failed: " },
    { Lang::Id::MultiSelect,    L"选中了多个文件，请只选择一个文件。", L"Select a single file." },
    { Lang::Id::Exported,       L"导出成功",      L"Exported" },
    { Lang::Id::ExportFail,     L"导出失败：无法写入文件", L"Export failed: cannot write." },
    { Lang::Id::Match,          L"匹配：%s",      L"Match: %s" },
    { Lang::Id::NoMatch,        L"不匹配",        L"No match" },
    { Lang::Id::Waiting,        L"等待计算完成…", L"Waiting…" },
    { Lang::Id::NotComputed,    L"尚未计算",      L"Not computed" },
    { Lang::Id::CannotThread,   L"无法创建计算线程", L"Cannot create thread" },
    { Lang::Id::ComputeFirst,   L"请先完成计算",  L"Compute first" },
    { Lang::Id::CopySelected,   L"复制选中",      L"Copy selected" },
    { Lang::Id::CopyAll,        L"复制全部",      L"Copy all" },
    { Lang::Id::Export,         L"导出",          L"Export" },
    { Lang::Id::Cancel,         L"取消",          L"Cancel" },
    { Lang::Id::Recompute,      L"重新计算",      L"Recompute" },
    { Lang::Id::AlgoColumn,     L"算法",          L"Algorithm" },
    { Lang::Id::HashColumn,     L"哈希值",        L"Hash" },
    { Lang::Id::Bytes,          L"%llu 字节",     L"%llu bytes" },
    { Lang::Id::Compute,        L"计算",          L"Compute" },
    { Lang::Id::Changed,        L"⚠ 文件在计算期间被修改，结果不可用于完整性校验。",
                                L"⚠ File changed while hashing; result is not usable for integrity checks." },
    { Lang::Id::ChangedSize,    L"⚠ 文件在计算期间被修改（%llu → %llu 字节），结果不可用。",
                                L"⚠ File changed while hashing (%llu → %llu bytes); result is not usable." },
    { Lang::Id::ChangeUnknown,  L"⚠ 无法读取文件状态，不能确认计算期间未被修改。",
                                L"⚠ Could not read file state; cannot confirm the file was unchanged." },
    { Lang::Id::ClipboardBusy,  L"剪贴板被其它程序占用，复制失败", L"Clipboard is busy; copy failed" },

    // 带单位的数值：%.1f <unit> (%llu bytes)
    { Lang::Id::SizeWithUnit,   L"%.1f %s (%llu 字节)", L"%.1f %s (%llu bytes)" },

    { Lang::Id::FilterSums,     L"哈希文件 (*.SUMS.txt)", L"Hash files (*.SUMS.txt)" },
    { Lang::Id::FilterAll,      L"所有文件 (*.*)",    L"All files (*.*)" },

    { Lang::Id::SourceIsTarget, L"不能覆盖正在计算的文件，请换一个文件名。",
                                L"Cannot overwrite the file being hashed; choose another name." },
    { Lang::Id::ExportWriteFail, L"导出失败：写入未完成（磁盘已满或权限不足）",
                                L"Export failed: write incomplete (disk full or access denied)" },

    { Lang::Id::ErrAlgoInit,    L"无法初始化哈希算法", L"Cannot initialize hash algorithm" },
    { Lang::Id::ErrHashObject,  L"无法创建哈希对象",   L"Cannot create hash object" },
    { Lang::Id::ErrHashData,    L"哈希计算失败",       L"Hash computation failed" },
    { Lang::Id::ErrOutOfMemory, L"内存不足，无法开始计算", L"Out of memory" },
    { Lang::Id::ErrException,   L"计算过程中发生异常", L"Exception during computation" },
    { Lang::Id::ErrWinErrorFmt, L"错误 %lu",           L"Error %lu" },
    { Lang::Id::WinErrorFmt,    L"%s: %s",             L"%s: %s" },

    // 设置区 / 算法勾选
    { Lang::Id::AlgoCRC32,       L"CRC32",              L"CRC32" },
    { Lang::Id::AlgoMD5,         L"MD5",                L"MD5" },
    { Lang::Id::AlgoSHA1,        L"SHA-1",              L"SHA-1" },
    { Lang::Id::AlgoSHA256,      L"SHA-256",            L"SHA-256" },
    { Lang::Id::AlgoSHA512,      L"SHA-512",            L"SHA-512" },
    { Lang::Id::NoAlgoSelected,  L"请至少勾选一个算法", L"Select at least one algorithm" },

    // 独立设置对话框（1.4）：标题/分组/确定取消；「大写显示」文案
    { Lang::Id::SettingsTitle,   L"设置",               L"Settings" },
    { Lang::Id::AlgoGroup,       L"算法",               L"Algorithms" },
    { Lang::Id::DisplayGroup,    L"显示",               L"Display" },
    { Lang::Id::DlgOk,           L"确定",               L"OK" },
    { Lang::Id::DlgCancel,       L"取消",               L"Cancel" },
    { Lang::Id::MenuUppercase,   L"大写显示",           L"Uppercase" },

    // 交互反馈（1.4 UI 优化）
    { Lang::Id::VerifyCue,       L"粘贴哈希值进行校验…", L"Paste a hash to verify…" },
    { Lang::Id::Copied,          L"已复制",              L"Copied" },
    { Lang::Id::FileSelectTip,   L"选择要查看的文件",    L"Select the file to show" },
    { Lang::Id::CopySelTip,      L"选中列表一行后复制（Ctrl+C）", L"Select a row to copy (Ctrl+C)" },

    // 多选（1.3）
    { Lang::Id::MultiTooMany,
        L"选中的文件过多（最多 64 个），已截断。",
        L"Too many files selected (max 64); truncated." },
    { Lang::Id::FileOf,          L"文件 %u/%u：%s",      L"File %u/%u: %s" },
    { Lang::Id::FileFailedOf,    L"文件 %u/%u 失败：%s",  L"File %u/%u failed: %s" },
    { Lang::Id::AllDone,         L"完成，用时 %.1f 秒",   L"Done in %.1f s" },
    { Lang::Id::AllFailed,       L"全部失败",             L"All failed" },
    { Lang::Id::MultiExportHint,
        L"已导出 %u 个文件的结果",
        L"Exported results for %u files" },
    { Lang::Id::PrevFile,       L"◀",                  L"◀" },
    { Lang::Id::NextFile,       L"▶",                  L"▶" },
};

// 只读原始配置值：LangId 可写 LCID 或 LANGID（低 16 位含义一致）。
// 返回 0 表示未配置，应回退到系统默认 UI 语言。
static DWORD ReadRawLangId() {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LG_Hash", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return 0;
    DWORD value = 0, sz = sizeof(value);
    const bool ok = RegQueryValueExW(h, L"LangId", 0, nullptr,
                                     reinterpret_cast<LPBYTE>(&value), &sz) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok ? value : 0;
}

// 把原始配置值应用到进程级状态。raw 为 0 时回退到系统默认语言。
static void ApplyRawLangId(DWORD raw) {
    LANGID langid = raw ? LANGIDFROMLCID(static_cast<LCID>(raw)) : 0;
    if (langid == 0) langid = GetSystemDefaultUILanguage();

    g_chinese.store(PRIMARYLANGID(langid) == LANG_CHINESE, std::memory_order_relaxed);
    g_rawLangId.store(raw, std::memory_order_relaxed);
    g_initialized.store(true, std::memory_order_release);
}

void Lang::Init() {
    ApplyRawLangId(ReadRawLangId());
}

// 与 Theme::Refresh() 对称：属性页 / 设置对话框创建时调用，让语言改动
// 「下次打开属性页」即生效，不必重启 explorer.exe。
//
// 只在配置值真的变化时切换；没变时开销仅一次注册表读，Get() 热路径不受影响
// （Get 仍然只读原子变量，不碰注册表）。
void Lang::Refresh() {
    const DWORD raw = ReadRawLangId();
    if (g_initialized.load(std::memory_order_acquire) &&
        raw == g_rawLangId.load(std::memory_order_relaxed)) {
        return;   // 未变化
    }
    ApplyRawLangId(raw);
}

// 首次取字符串时才读语言设置：避免在 DllMain 的加载器锁内访问注册表
static void EnsureInit() {
    if (!g_initialized.load(std::memory_order_acquire)) Lang::Init();
}

const wchar_t* Lang::Get(Id id) {
    EnsureInit();
    const bool zh = g_chinese.load(std::memory_order_relaxed);
    for (const auto& e : kTable)
        if (e.id == id) return zh ? e.zh : e.en;
    return L"";
}

bool Lang::IsChinese() {
    EnsureInit();
    return g_chinese.load(std::memory_order_relaxed);
}
