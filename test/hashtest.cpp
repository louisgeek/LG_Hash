#include "../src/HashEngine.h"
#include <cstdio>
#include <string>
#include <vector>

// 命令行哈希校验器：直接调用 HashEngine，用于交叉验证 UI 之外的算法输出。
//   hashtest.exe <file>        打印五种摘要，格式 "NAME hex"
//   hashtest.exe --cancel <f>  立即置位取消事件，期望返回“已取消”且无结果
//   hashtest.exe --sharing <f> 独占打开文件后仍应能算出摘要
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: hashtest [--cancel|--sharing] <file>\n");
        return 2;
    }

    const std::wstring first = argv[1];
    const bool cancelMode = (first == L"--cancel");
    const bool sharingMode = (first == L"--sharing");
    if ((cancelMode || sharingMode) && argc < 3) {
        wprintf(L"usage: hashtest [--cancel|--sharing] <file>\n");
        return 2;
    }
    const std::wstring path = (cancelMode || sharingMode) ? argv[2] : first;

    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel) {
        wprintf(L"cannot create cancel event\n");
        return 1;
    }

    if (cancelMode) {
        SetEvent(cancel);
        std::wstring hex[kHashCount];
        std::wstring err = ComputeFileHashes(path, cancel, nullptr, hex);
        CloseHandle(cancel);
        const bool cancelled = (err == kCancelledResult);
        const bool empty = hex[0].empty();
        wprintf(L"cancelled=%d no-result=%d\n", cancelled ? 1 : 0, empty ? 1 : 0);
        return (cancelled && empty) ? 0 : 1;
    }

    // 以读写方式持有文件句柄，验证扩展用 FILE_SHARE_* 仍能读取
    HANDLE hold = INVALID_HANDLE_VALUE;
    if (sharingMode) {
        hold = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
        if (hold == INVALID_HANDLE_VALUE) {
            wprintf(L"hold-open-failed %lu\n", GetLastError());
            CloseHandle(cancel);
            return 1;
        }
    }

    std::wstring hex[kHashCount];
    std::wstring err = ComputeFileHashes(path, cancel, nullptr, hex);
    if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
    CloseHandle(cancel);

    if (!err.empty()) {
        wprintf(L"ERROR %s\n", err.c_str());
        return 1;
    }
    for (int i = 0; i < kHashCount; ++i)
        wprintf(L"%s %s\n", kHashAlgos[i].name, hex[i].c_str());
    return 0;
}
