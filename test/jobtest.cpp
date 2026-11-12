#include "../src/HashJob.h"
#include "../src/HashEngine.h"

#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>

// 多选支持（1.3）的 Job 层测试：验证 HashJob（从 HashPage.cpp 拆出的
// 计算任务层）对多文件的串行计算、结果数组与取消语义。
//
// 为什么不做窗口级 UI 测试：从外部驱动 explorer 多选属性表在测试机上
// 不稳定（COM 调用方无消息泵 / /separate explorer 中途夭折 / SendKeys
// 需要真实前台焦点）。UI 渲染路径（RenderResults/SwitchFile/导航按钮）
// 与单文件共用同一套函数，由 verify_tab.ps1 覆盖；这里只测拆出来的
// 纯逻辑层——与 hashtest 直接链接引擎是同一思路。
//
//   1) 两个文件串行计算，results 与路径一一对应
//   2) 结果与 CNG 对内存缓冲的独立计算值一致
//   3) 预置取消事件时整体返回取消、不写结果
//   4) StopJob 置 alive=false 后线程安全收尾（不死锁、不崩溃）

#pragma comment(lib, "bcrypt.lib")

namespace {

bool Sha256Hex(const BYTE* data, DWORD len, std::wstring* out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    // 对象缓冲必须按 CNG 报告的长度动态分配（栈上定长缓冲会被写穿）
    DWORD objLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                      sizeof(objLen), &cb, 0);
    std::vector<BYTE> obj(objLen);
    bool ok = false;
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) >= 0) {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), len, 0) >= 0) {
            BYTE digest[32] = {};
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
                static const wchar_t digits[] = L"0123456789abcdef";
                std::wstring s;
                for (BYTE b : digest) {
                    s += digits[b >> 4];
                    s += digits[b & 0xF];
                }
                *out = s;
                ok = true;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool WriteAll(const std::wstring& path, const std::wstring& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(h, content.c_str(),
                              static_cast<DWORD>(content.size() * 2), &w, nullptr);
    CloseHandle(h);
    return ok && w == content.size() * 2;
}

} // namespace

int wmain() {
    // 崩溃时缓冲输出会丢失，禁用缓冲以精确定位
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return 1;
    const std::wstring dir = std::wstring(tmp) + L"hashpage_job_";
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring data1 = L"multi-select test file ONE\r\n";
    const std::wstring data2 = L"multi-select test file TWO with more content\r\n";
    const std::wstring f1 = dir + L"\\alpha.txt";
    const std::wstring f2 = dir + L"\\beta.txt";
    if (!WriteAll(f1, data1) || !WriteAll(f2, data2)) {
        wprintf(L"FAIL: cannot create test files\n");
        return 1;
    }

    std::wstring shaF1, shaF2;
    if (!Sha256Hex(reinterpret_cast<const BYTE*>(data1.c_str()),
                   static_cast<DWORD>(data1.size() * 2), &shaF1) ||
        !Sha256Hex(reinterpret_cast<const BYTE*>(data2.c_str()),
                   static_cast<DWORD>(data2.size() * 2), &shaF2)) {
        wprintf(L"FAIL: cannot compute expected digests\n");
        return 1;
    }

    int failures = 0;
    bool want[kHashCount] = { true, true, true, true, true };

    // 1) 多文件串行计算
    {
        Job* job = CreateJob(nullptr, 1, { f1, f2 }, want);
        if (!job) {
            wprintf(L"FAIL: CreateJob\n");
            return 1;
        }
        if (!StartWorker(job)) {
            wprintf(L"FAIL: StartWorker\n");
            StopJob(job);
            return 1;
        }
        // 等工作者线程跑完（它收尾时 Release，Job 若还活着说明 UI 侧引用还在）
        for (int i = 0; i < 300; ++i) {
            EnterCriticalSection(&job->cs);
            const size_t done = job->results.size() == 2 &&
                                job->results[0].finished &&
                                job->results[1].finished;
            LeaveCriticalSection(&job->cs);
            if (done) break;
            Sleep(100);
        }
        EnterCriticalSection(&job->cs);
        const bool both = job->results.size() == 2 &&
                          job->results[0].finished && job->results[1].finished;
        const std::wstring r1 = job->results[0].hex[3];
        const std::wstring r2 = job->results[1].hex[3];
        LeaveCriticalSection(&job->cs);
        StopJob(job);

        wprintf(L"serial-compute ok=%d\n", both ? 1 : 0);
        if (!both) ++failures;
        wprintf(L"file1 sha256 ok=%d\n", r1 == shaF1 ? 1 : 0);
        if (r1 != shaF1) ++failures;
        wprintf(L"file2 sha256 ok=%d\n", r2 == shaF2 ? 1 : 0);
        if (r2 != shaF2) ++failures;
    }

    // 2) 预置取消：整体取消、不写结果
    {
        Job* job = CreateJob(nullptr, 2, { f1, f2 }, want);
        SetEvent(job->cancel);
        if (StartWorker(job)) {
            for (int i = 0; i < 100; ++i) {
                EnterCriticalSection(&job->cs);
                bool pending = job->results[0].error.empty() && job->results[1].error.empty();
                LeaveCriticalSection(&job->cs);
                if (!pending) break;
                Sleep(50);
            }
            EnterCriticalSection(&job->cs);
            bool cancelled = !job->results[0].finished && !job->results[1].finished;
            bool noHex = job->results[0].hex[0].empty() && job->results[1].hex[0].empty();
            LeaveCriticalSection(&job->cs);
            StopJob(job);
            wprintf(L"pre-cancelled ok=%d no-results=%d\n", cancelled ? 1 : 0, noHex ? 1 : 0);
            if (!cancelled || !noHex) ++failures;
        } else {
            StopJob(job);
            wprintf(L"FAIL: StartWorker (cancel case)\n");
            ++failures;
        }
    }

    // 3) 部分失败：一个存在、一个不存在
    {
        Job* job = CreateJob(nullptr, 3, { f1, dir + L"\\missing.bin" }, want);
        if (StartWorker(job)) {
            for (int i = 0; i < 300; ++i) {
                EnterCriticalSection(&job->cs);
                const bool settled = !job->results[0].error.empty() ||
                                     job->results[0].finished;
                const bool settled2 = !job->results[1].error.empty() ||
                                      job->results[1].finished;
                LeaveCriticalSection(&job->cs);
                if (settled && settled2) break;
                Sleep(100);
            }
            EnterCriticalSection(&job->cs);
            const bool okFirst = job->results[0].finished;
            const bool errSecond = !job->results[1].finished &&
                                   !job->results[1].error.empty() &&
                                   job->results[1].error != kCancelledResult;
            LeaveCriticalSection(&job->cs);
            StopJob(job);
            wprintf(L"partial-failure first-ok=%d second-error=%d\n",
                    okFirst ? 1 : 0, errSecond ? 1 : 0);
            if (!okFirst || !errSecond) ++failures;
        } else {
            StopJob(job);
            wprintf(L"FAIL: StartWorker (partial case)\n");
            ++failures;
        }
    }

    DeleteFileW(f1.c_str());
    DeleteFileW(f2.c_str());
    RemoveDirectoryW(dir.c_str());

    wprintf(L"failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
