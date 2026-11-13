#pragma once

#include <windows.h>

namespace Lang {
    enum class Id {
        PageTitle, VerifyGroup, Computing, Done, Cancelled, Failed,
        MultiSelect, Exported, ExportFail, Match, NoMatch, Waiting, NotComputed,
        CannotThread, ComputeFirst, CopySelected, CopyAll, Export, Cancel, Recompute,
        AlgoColumn, HashColumn, Bytes,
        // 按需计算 / 变更提示 / 剪贴板
        Compute, Changed, ChangedSize, ChangeUnknown, ClipboardBusy,
        // 单位与数值格式
        SizeWithUnit,
        // 文件对话框过滤器
        FilterSums, FilterAll,
        // 导出保护
        SourceIsTarget, ExportWriteFail,
        // 错误信息（引擎与 UI 共用，避免英文界面冒出中文）
        ErrAlgoInit, ErrHashObject, ErrHashData, ErrOutOfMemory, ErrException,
        ErrWinErrorFmt, WinErrorFmt,
        // 设置区 / 算法勾选
        AlgoCRC32, AlgoMD5, AlgoSHA1, AlgoSHA256, AlgoSHA512,
        NoAlgoSelected,
        // 独立设置对话框（1.4）：标题/分组/确定取消；「大写显示」文案
        SettingsTitle, AlgoGroup, DisplayGroup, DlgOk, DlgCancel,
        MenuUppercase,
        // 交互反馈（1.4 UI 优化）
        VerifyCue, Copied, FileSelectTip, CopySelTip,
        // 多选（1.3）
        MultiTooMany, FileOf, FileFailedOf, AllDone, AllFailed, MultiExportHint,
        PrevFile, NextFile,
    };

    // 首次 Get() 时按需初始化；之后返回当前语言字符串
    void Init();
    const wchar_t* Get(Id id);
    bool IsChinese();

    // 重新读取语言设置。与 Theme::Refresh() 对称：在属性页/设置对话框创建时调用，
    // 使「改完设置，下次打开属性页」即可生效，不必重启 explorer.exe。
    //
    // 内部按注册表原始值做比较，值没变时直接返回——Get() 热路径仍然不碰注册表，
    // 每次打开属性页只多一次轻量读，不会影响列表渲染性能。
    void Refresh();
}
