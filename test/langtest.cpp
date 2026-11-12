#include "../src/Lang.h"
#include <cstdio>

// 语言表自检：确认每个 Id 都返回非空文案，并打印关键项。
// 语言由 HKCU\Software\LG_Hash\LangId（LCID 或 LANGID）或系统默认 UI 语言决定。
int wmain() {
    const Lang::Id ids[] = {
        Lang::Id::PageTitle, Lang::Id::VerifyGroup, Lang::Id::Computing, Lang::Id::Done,
        Lang::Id::Cancelled, Lang::Id::Failed, Lang::Id::MultiSelect, Lang::Id::Exported,
        Lang::Id::ExportFail, Lang::Id::Match, Lang::Id::NoMatch, Lang::Id::Waiting,
        Lang::Id::NotComputed, Lang::Id::CannotThread, Lang::Id::ComputeFirst,
        Lang::Id::CopySelected, Lang::Id::CopyAll, Lang::Id::Export, Lang::Id::Cancel,
        Lang::Id::Recompute, Lang::Id::AlgoColumn, Lang::Id::HashColumn, Lang::Id::Bytes,
        Lang::Id::Compute, Lang::Id::Changed, Lang::Id::ChangedSize,
        Lang::Id::ChangeUnknown, Lang::Id::ClipboardBusy,
        Lang::Id::SizeWithUnit, Lang::Id::FilterSums, Lang::Id::FilterAll,
        Lang::Id::SourceIsTarget, Lang::Id::ExportWriteFail,
        Lang::Id::ErrAlgoInit, Lang::Id::ErrHashObject, Lang::Id::ErrHashData,
        Lang::Id::ErrOutOfMemory, Lang::Id::ErrException,
        Lang::Id::ErrWinErrorFmt, Lang::Id::WinErrorFmt,
        // 设置 / 勾选 / 独立设置对话框
        Lang::Id::AlgoCRC32, Lang::Id::AlgoMD5, Lang::Id::AlgoSHA1,
        Lang::Id::AlgoSHA256, Lang::Id::AlgoSHA512, Lang::Id::NoAlgoSelected,
        Lang::Id::SettingsTitle, Lang::Id::AlgoGroup, Lang::Id::DisplayGroup,
        Lang::Id::DlgOk, Lang::Id::DlgCancel,
        Lang::Id::MenuUppercase,
        // 交互反馈（1.4 UI 优化）
        Lang::Id::VerifyCue, Lang::Id::Copied, Lang::Id::FileSelectTip, Lang::Id::CopySelTip,
        // 多选（1.3）
        Lang::Id::MultiTooMany, Lang::Id::FileOf, Lang::Id::FileFailedOf,
        Lang::Id::AllDone, Lang::Id::AllFailed, Lang::Id::MultiExportHint,
        Lang::Id::PrevFile, Lang::Id::NextFile,
    };
    const int n = static_cast<int>(sizeof(ids) / sizeof(ids[0]));

    int problems = 0;
    for (int i = 0; i < n; ++i) {
        const wchar_t* s = Lang::Get(ids[i]);
        if (!s || !*s) {
            wprintf(L"EMPTY entry at index %d\n", i);
            ++problems;
        }
    }

    wprintf(L"entries=%d chinese=%d problems=%d\n",
            n, Lang::IsChinese() ? 1 : 0, problems);
    wprintf(L"Export=[%s] CopyAll=[%s] Done=[%s]\n",
            Lang::Get(Lang::Id::Export),
            Lang::Get(Lang::Id::CopyAll),
            Lang::Get(Lang::Id::Done));
    return problems == 0 ? 0 : 1;
}
