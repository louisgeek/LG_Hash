#pragma once

// 属性页 UI 的内部共享声明。只给 LG_Hash*.cpp 三个实现文件用，不属于 DLL 对外接口，
// 也不要被 HashExt / installer / test 包含。
//
// 拆分前这些声明都在 LG_Hash.cpp 内部，PageState 是单文件私有的；按「主流程 /
// 对话框与导出 / 自绘」拆成三个 TU 后，它必须提升到这里。这是本次拆分唯一
// 牺牲的封装性——换来的是每个文件职责单一，改动的爆炸半径可判断。
//
// 只放真正跨文件使用的声明。只在单个 .cpp 内用到的辅助函数继续留在原地保持
// static，否则内部链接的好处（不污染符号、编译器可内联）就白丢了。

#include <windows.h>
#include <commctrl.h>
#include <string>

#include "HashEngine.h"
#include "HashJob.h"
#include "LG_Hash.h"

#define TIMER_EXPORT    0x1001
#define TIMER_CLIPBOARD 0x1002

// 属性页的每页运行时状态
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

// 从 Job 里抄出当前文件（下标 idx）的结果快照。调用方必须已持 job 判空；
// 在 cs 内复制，出锁后再用，避免持锁调用 UI。
struct ResultSnap {
    std::wstring hex[kHashCount];
    FileChangeInfo change;
    bool finished = false;
    std::wstring error;          // kCancelledResult 表示已取消
    double elapsed = 0;
};

// 自绘菜单项：所有弹出菜单统一走 MF_OWNERDRAW。
// 浅色下模仿系统菜单配色；深色下套页面主题（系统菜单不跟随暗色，
// 直接用 TrackPopupMenu 会在深色页面上开出一块浅色菜单）。
struct MenuEntry {
    const wchar_t* text;   // nullptr = 分隔线
    UINT id;               // WM_COMMAND 命令 ID
    bool checked;
    bool underline;        // 文字下画线（表示可点击的外链）
};

// ---- 主流程 → 对话框 / 自绘 ----
void SetStatus(HWND hwnd, const std::wstring& text);
void UpdateCopySelState(HWND hwnd, PageState* st);
void SnapCurrent(Job* job, size_t idx, ResultSnap* out);
std::wstring BuildAllLines(HWND hwnd, const wchar_t* fileName, const bool algo[kHashCount]);
void RebuildList(HWND hwnd, PageState* st, const wchar_t* placeholder);
void EvaluateVerify(HWND hwnd, PageState* st);
void RenderCurrent(HWND hwnd, PageState* st);

// ---- 对话框 → 主流程 ----
void LoadSettings(PageState* st);
void OpenSettings(HWND hwnd, PageState* st);
void DoExport(HWND hwnd, PageState* st);

// ---- 自绘 / 菜单 → 主流程 ----
HFONT EnsureMenuFont(PageState* st);
void DrawMenuEntry(const DRAWITEMSTRUCT* dis, HFONT font);
void ShowFilesMenu(HWND hwnd, PageState* st);
void OpenProjectPage(HWND hwnd);
LRESULT CustomDrawHeader(LPNMCUSTOMDRAW cd);
void SetVerifyCue(HWND hwnd, PageState* st, bool show);
void ApplyLanguage(HWND hwnd);
void EnableOwnerDraw(HWND page);
LRESULT DrawOwnerButton(const DRAWITEMSTRUCT* dis);
