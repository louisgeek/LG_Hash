#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "HashEngine.h"

// 计算任务与工作者线程从属性页 UI 里拆出来：这一层只负责
// 「读文件、算哈希、回报进度」，完全不碰控件；UI 侧只消费消息。

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

// 进度消息的 wParam 布局（64 位）：
//   低 16 位  百分比 0..100
//   中 16 位  文件下标（多选串行计算时的当前文件）
//   高 32 位  任务序号 seq（丢弃旧任务消息用，见 PageState::jobSeq）
// lParam 恒为 Job*。

// 单个文件的计算结果。多选时 Job 内持有与 paths 一一对应的数组。
struct FileResult {
    std::wstring hex[kHashCount];  // 空串 = 未算出（未勾选 / 失败 / 取消）
    FileChangeInfo change;
    std::wstring error;            // 空 = 成功；kCancelledResult = 已取消
    bool finished = false;
};

// 后台计算任务：对话框与工作者线程各持一份引用（ref 初值 2）。
// 先放手的一方（关窗）置 alive=false 并触发取消事件，后放手的一方负责 delete。
// results 等字段由 worker 写、UI 读，两侧都必须持 cs 访问。
struct Job {
    LONG ref = 2;                   // 对话框与工作者线程各持一份
    CRITICAL_SECTION cs;
    HWND hwnd = nullptr;
    bool alive = true;
    HANDLE cancel = nullptr;
    UINT seq = 0;                   // 任务序号：标识作用见 PageState::jobSeq
    std::vector<std::wstring> paths; // 待算文件；多选时不止一个，串行计算
    bool want[kHashCount] = { true, true, true, true, true };
    std::vector<FileResult> results; // 与 paths 一一对应
    ULONGLONG startTick = 0;
    double elapsedSec = 0;

    void AddRef() { InterlockedIncrement(&ref); }
    void Release() {
        if (InterlockedDecrement(&ref) == 0) {
            DeleteCriticalSection(&cs);
            CloseHandle(cancel);
            delete this;
        }
    }
    void Post(UINT msg, WPARAM w = 0, LPARAM l = 0) {
        EnterCriticalSection(&cs);
        if (alive) PostMessageW(hwnd, msg, w, l);
        LeaveCriticalSection(&cs);
    }
};

// 创建任务（含 cs / 取消事件 / 结果数组初始化）。失败返回 nullptr，内部自清理。
Job* CreateJob(HWND hwnd, UINT seq, const std::vector<std::wstring>& paths,
               const bool want[kHashCount]);

// 起工作者线程并转交线程侧引用；失败返回 false（Job 仍由调用方 StopJob 释放）。
bool StartWorker(Job* job);

// 请求取消并释放 UI 侧引用；调用方应随即把自己的 Job* 置空。
void StopJob(Job* job);
