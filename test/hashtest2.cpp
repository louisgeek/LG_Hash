#include "../src/HashEngine.h"
#include <cstdio>
#include <string>

// 引擎边界自检：覆盖 UI 不便触发的分支，全部用同一份 ComputeFileHashes 实现。
//   1) 空文件       —— 进度分母为 0 时应按 100% 处理，摘要取空串的哈希
//   2) 恰好 1MB     —— 分块边界
//   3) 1MB + 1 字节 —— 跨块尾段
//   4) 取消         —— 预先置位事件，应返回 kCancelledResult 且不写结果
//
// 测试文件写在系统临时目录，跑完即删，不污染仓库根目录。
int wmain() {
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        wprintf(L"cannot get temp path\n");
        return 1;
    }
    const std::wstring base = std::wstring(tmp) + L"lg_hash_edge_";
    const std::wstring names[3] = { base + L"empty.bin", base + L"1mb.bin", base + L"1mb1.bin" };
    const int sizes[3] = { 0, 1 << 20, (1 << 20) + 1 };

    int failures = 0;

    for (int i = 0; i < 3; ++i) {
        HANDLE f = CreateFileW(names[i].c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            wprintf(L"cannot create test file %d (err=%lu)\n", i, GetLastError());
            return 1;
        }
        std::string data(static_cast<size_t>(sizes[i]), '\0');
        for (size_t k = 0; k < data.size(); ++k)
            data[k] = static_cast<char>((k * 31 + 7) & 0xFF);
        DWORD written = 0;
        if (!data.empty())
            WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(f);

        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::wstring hex[kHashCount];
        std::wstring err = ComputeFileHashes(names[i], cancel, nullptr, hex);
        CloseHandle(cancel);

        const bool ok = err.empty() && hex[0].size() == 8 && hex[4].size() == 128;
        wprintf(L"case %d size=%d ok=%d crc32=%s\n", i, sizes[i], ok ? 1 : 0, hex[0].c_str());
        if (!ok) ++failures;
    }

    // 取消分支
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetEvent(cancel);
    std::wstring hex[kHashCount];
    std::wstring err = ComputeFileHashes(names[1], cancel, nullptr, hex);
    CloseHandle(cancel);
    const bool cancelOk = (err == kCancelledResult) && hex[0].empty();
    wprintf(L"cancel ok=%d\n", cancelOk ? 1 : 0);
    if (!cancelOk) ++failures;

    // 部分勾选：只勾 CRC32 + SHA-256（bit0、bit3）
    {
        bool want[kHashCount] = { true, false, false, true, false };
        std::wstring part[kHashCount];
        HANDLE c2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const std::wstring e2 = ComputeFileHashes(names[1], c2, nullptr, part, nullptr, want);
        CloseHandle(c2);
        const bool okSel = e2.empty() && part[0].size() == 8 && part[3].size() == 64 &&
                           part[1].empty() && part[2].empty() && part[4].empty();
        wprintf(L"select crc32+sha256 ok=%d crc=%s sha256len=%zu\n",
                okSel ? 1 : 0, part[0].c_str(), part[3].size());
        if (!okSel) ++failures;
    }

    // 全不勾：应按取消处理且不写任何结果
    {
        bool none[kHashCount] = { false, false, false, false, false };
        std::wstring part[kHashCount];
        HANDLE c3 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const std::wstring e3 = ComputeFileHashes(names[1], c3, nullptr, part, nullptr, none);
        CloseHandle(c3);
        bool empty = true;
        for (int i = 0; i < kHashCount; ++i) empty = empty && part[i].empty();
        const bool okNone = (e3 == kCancelledResult) && empty;
        wprintf(L"select-none ok=%d\n", okNone ? 1 : 0);
        if (!okNone) ++failures;
    }

    // 收尾：删掉临时文件
    for (int i = 0; i < 3; ++i) DeleteFileW(names[i].c_str());

    // ---------------------------------------------------------------- 变更检测加固（1.3）
    // 场景：计算期间原地改写同长度内容，再把 LastWriteTime 手工改回原值。
    // 旧实现（只比大小+LastWrite）会漏判；新实现靠 NTFS ChangeTime 兜住。
    {
        const std::wstring path = base + L"stealth.bin";
        // 8 MiB：够大以覆盖计算窗口，又不至于让测试拖太久
        const int sz = 8 << 20;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            wprintf(L"cannot create stealth.bin (err=%lu)\n", GetLastError());
            return 1;
        }
        std::string data(static_cast<size_t>(sz), 'A');
        DWORD w = 0;
        WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);

        FILETIME orig{};
        GetFileTime(f, nullptr, nullptr, &orig);

        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        FileChangeInfo change{};
        std::wstring hex[kHashCount];
        // 在读到一半时（进度 50%）原地改写并还原时间戳
        std::wstring err = ComputeFileHashes(
            path, cancel,
            [&](int pct) {
                if (pct == 50) {
                    // 原地写同长度内容：大小不变
                    std::string patch(static_cast<size_t>(sz), 'B');
                    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
                    DWORD pw = 0;
                    WriteFile(f, patch.data(), static_cast<DWORD>(patch.size()), &pw, nullptr);
                    // 把 LastWriteTime 改回原值——模拟「时间戳被还原」的改写
                    SetFileTime(f, nullptr, nullptr, &orig);
                }
            },
            hex, &change);
        CloseHandle(cancel);
        CloseHandle(f);
        DeleteFileW(path.c_str());

        const bool caught = err.empty() && change.valid && change.changed;
        wprintf(L"stealth-rewrite caught=%d (changed=%d valid=%d changeTime=%llu->%llu)\n",
                caught ? 1 : 0, change.changed ? 1 : 0, change.valid ? 1 : 0,
                static_cast<unsigned long long>(change.changeBefore),
                static_cast<unsigned long long>(change.changeAfter));
        if (!caught) ++failures;
        // ChangeTime 路径不可用（非 NTFS / 旧系统）时允许降级跳过：
        // 前后都为 0 表示取不到，此时该项不算失败
        if (!caught && change.changeBefore == 0 && change.changeAfter == 0) {
            wprintf(L"  (ChangeTime unavailable on this volume - case skipped)\n");
            --failures;
        }
    }

    wprintf(L"failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
