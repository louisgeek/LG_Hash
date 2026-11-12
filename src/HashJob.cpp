#include "HashJob.h"
#include "Lang.h"

#include <new>

namespace {

DWORD WINAPI WorkerProc(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    // 异常绝不能逃出线程入口：那会直接把 explorer 进程带走
    try {
        bool want[kHashCount];
        std::vector<std::wstring> paths;
        {
            EnterCriticalSection(&job->cs);
            for (int i = 0; i < kHashCount; ++i) want[i] = job->want[i];
            paths = job->paths;
            LeaveCriticalSection(&job->cs);
        }

        bool cancelled = false;
        for (size_t i = 0; i < paths.size(); ++i) {
            std::wstring hex[kHashCount];
            FileChangeInfo change;
            std::wstring err = ComputeFileHashes(
                paths[i], job->cancel,
                [job, i](int pct) {
                    // 低 16 位百分比，中 16 位文件下标，高 32 位任务序号
                    const WPARAM w = (static_cast<WPARAM>(job->seq) << 32) |
                                     (static_cast<WPARAM>(i) << 16) |
                                     static_cast<WPARAM>(static_cast<WORD>(pct));
                    job->Post(WM_APP_PROGRESS, w, reinterpret_cast<LPARAM>(job));
                },
                hex, &change, want);

            if (err == kCancelledResult) {
                cancelled = true;
                break;   // 取消：不写结果，已完成文件的结果保留
            }

            EnterCriticalSection(&job->cs);
            if (i < job->results.size()) {
                for (int a = 0; a < kHashCount; ++a) job->results[i].hex[a] = hex[a];
                job->results[i].change = change;
                job->results[i].error = err;
                job->results[i].finished = err.empty();
            }
            LeaveCriticalSection(&job->cs);
        }

        EnterCriticalSection(&job->cs);
        job->elapsedSec = static_cast<double>(GetTickCount64() - job->startTick) / 1000.0;
        if (cancelled && !job->results.empty()) {
            // 让 UI 能识别「已取消」：给一个哨兵错误（不覆盖已有真实错误）
            for (auto& r : job->results) {
                if (!r.finished && r.error.empty()) r.error = kCancelledResult;
            }
        }
        LeaveCriticalSection(&job->cs);
    } catch (...) {
        EnterCriticalSection(&job->cs);
        for (auto& r : job->results) {
            if (r.error.empty()) r.error = Lang::Get(Lang::Id::ErrException);
            r.finished = false;
        }
        LeaveCriticalSection(&job->cs);
    }

    job->Post(WM_APP_DONE, static_cast<WPARAM>(job->seq), reinterpret_cast<LPARAM>(job));
    job->Release();
    return 0;
}

} // namespace

Job* CreateJob(HWND hwnd, UINT seq, const std::vector<std::wstring>& paths,
               const bool want[kHashCount]) {
    if (paths.empty()) return nullptr;

    Job* job = new (std::nothrow) Job();
    if (!job) return nullptr;
    InitializeCriticalSection(&job->cs);
    job->cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->cancel) {
        DeleteCriticalSection(&job->cs);
        delete job;
        return nullptr;
    }
    job->hwnd = hwnd;
    job->seq = seq;
    job->paths = paths;
    for (int i = 0; i < kHashCount; ++i) job->want[i] = want[i];
    job->results.resize(paths.size());
    job->startTick = GetTickCount64();
    return job;
}

bool StartWorker(Job* job) {
    HANDLE th = CreateThread(nullptr, 0, WorkerProc, job, 0, nullptr);
    if (!th) return false;
    CloseHandle(th);
    return true;
}

void StopJob(Job* job) {
    if (!job) return;
    EnterCriticalSection(&job->cs);
    job->alive = false;
    LeaveCriticalSection(&job->cs);
    SetEvent(job->cancel);
    job->Release();
}
