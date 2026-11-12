#include "PathUtil.h"

#include <windows.h>

namespace {

// 取规范化后的最终路径（去 \\?\ 前缀）；失败返回空串
std::wstring Normalize(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    wchar_t buf[1024];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= 1024) return std::wstring();
    std::wstring s(buf, n);
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    return s;
}

} // namespace

bool PathUtil::SameFile(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    if (_wcsicmp(a.c_str(), b.c_str()) == 0) return true;

    const std::wstring na = Normalize(a);
    const std::wstring nb = Normalize(b);
    if (na.empty() || nb.empty()) return false;
    return _wcsicmp(na.c_str(), nb.c_str()) == 0;
}
