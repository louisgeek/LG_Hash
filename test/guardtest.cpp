#include "../src/PathUtil.h"
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>

// 导出防护的单测：SameFile 必须能识别「同一个文件的不同的写法」。
// 链接的是 src/PathUtil.cpp 里的真实实现（与属性页用的是同一份代码），
// 不再维护镜像副本，从根源上避免测试与实现漂移。
int wmain() {
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) return 1;
    const std::wstring dir = std::wstring(tmp) + L"lg_hash_guard_";
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring f1 = dir + L"\\data.txt";
    const std::wstring f2 = dir + L"\\other.txt";
    HANDLE h = CreateFileW(f1.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, "123456789", 9, &w, nullptr); CloseHandle(h); }
    h = CreateFileW(f2.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, "x", 1, &w, nullptr); CloseHandle(h); }

    struct Case { const wchar_t* label; std::wstring a, b; bool want; };
    const Case cases[] = {
        { L"identical path",        f1, f1, true },
        { L"same file, diff case",  f1, dir + L"\\DATA.TXT", true },
        { L"different file",        f1, f2, false },
        { L"empty operand",         f1, std::wstring(), false },
    };

    int failures = 0;
    for (const auto& c : cases) {
        const bool got = PathUtil::SameFile(c.a, c.b);
        wprintf(L"%-24s want=%d got=%d %s\n", c.label, c.want ? 1 : 0, got ? 1 : 0,
                got == c.want ? L"OK" : L"FAIL");
        if (got != c.want) ++failures;
    }

    DeleteFileW(f1.c_str());
    DeleteFileW(f2.c_str());
    RemoveDirectoryW(dir.c_str());
    wprintf(L"failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
