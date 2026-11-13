#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <fstream>
#include <string>
#include <vector>

#include "version.h"

#define IDR_LG_HASH_DLL   101
#define kUninstallKey      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LG_Hash"
#define kProductName       L"LG_Hash"
// 版本号唯一来源：src\version.h。卸载项「应用和功能」显示的就是它。
#define kVersion           LG_HASH_VERSION_SHORT

// 安装目录：%LOCALAPPDATA%\LG_Hash
static std::wstring GetInstallDir() {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path)))
        return std::wstring(path) + L"\\LG_Hash";
    return std::wstring();
}

static std::wstring ModulePath() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return std::wstring();
    if (n < MAX_PATH) return std::wstring(buf, n);
    // 路径被截断时换更大的缓冲重取（安装目录可能很深）
    std::wstring big(32768, L'\0');
    const DWORD got = GetModuleFileNameW(nullptr, big.data(), static_cast<DWORD>(big.size()));
    if (got == 0) return std::wstring();
    big.resize(got);
    return big;
}

static bool ExtractDll(const std::wstring& outPath) {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LG_HASH_DLL), RT_RCDATA);
    if (!hRes) return false;
    const DWORD size = SizeofResource(nullptr, hRes);
    void* data = LockResource(LoadResource(nullptr, hRes));
    if (!data || size == 0) return false;

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    return static_cast<bool>(out);
}

static bool RunRegsvr32(const std::wstring& dllPath, bool silent, DWORD* exitCode) {
    std::wstring params = std::wstring(silent ? L"/s " : L"") + L"\"" + dllPath + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"regsvr32.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = silent ? SW_HIDE : SW_SHOW;
    if (!ShellExecuteExW(&sei)) return false;
    if (!sei.hProcess) return true;

    const DWORD wait = WaitForSingleObject(sei.hProcess, 20000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (exitCode) *exitCode = code;
    return wait == WAIT_OBJECT_0 && code == 0;
}

static std::wstring UniquePath(const std::wstring& base) {
    wchar_t buf[MAX_PATH];
    swprintf_s(buf, MAX_PATH, L"%s_%08X.dll", base.c_str(), GetTickCount());
    return buf;
}

static void RegSetString(HKEY root, const wchar_t* sub, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

static void RegSetDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

// 「应用和功能」里的卸载入口。没有这一项，用户装完就只能靠记住命令行参数。
static void RegisterUninstallEntry(const std::wstring& installDir) {
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", kProductName);
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", kVersion);
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", kProductName);
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", installDir);
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", ModulePath());
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString",
                 L"\"" + ModulePath() + L"\" /uninstall");
    RegSetString(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString",
                 L"\"" + ModulePath() + L"\" /uninstall /quiet");
    RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1);
    RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);
    RegSetDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", 600);
}

static void RemoveUninstallEntry() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
}

// 升级时清理同目录下的旧 DLL：包括被占用时改用过的 LG_Hash_XXXXXXXX.dll
//
// 注意：这里只删文件，不调用 regsvr32 /u。
// 所有版本共用同一个 CLSID，DllUnregisterServer 会把整个 CLSID 树删掉；
// 若在注册新版之后反注册旧版，就会把新版的注册项一起抹去，升级后属性页直接消失。
// 旧 DLL 的注册信息会被新版的 DllRegisterServer 覆盖，本来就不需要单独清理。
static int CleanStaleDlls(const std::wstring& dir, const std::wstring& keep) {
    int removed = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\LG_Hash*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (!keep.empty() && _wcsicmp(full.c_str(), keep.c_str()) == 0) continue;
        // 删除失败（仍被 explorer 加载）就留着，下次升级或重启后再清
        if (DeleteFileW(full.c_str())) ++removed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

// 按「完整参数」匹配，而不是子串查找：
// 子串匹配会把 /uninstalldata 误判成 /uninstall，也会误中路径里含该字样的安装目录。
// 支持 /flag、-flag、--flag 三种写法，忽略大小写。
static bool HasFlag(LPCWSTR cmdLine, const wchar_t* flag) {
    if (!cmdLine || !flag) return false;

    std::wstring flagName(flag);
    // 去掉前导的 / 或 -，只留名字
    const size_t first = flagName.find_first_not_of(L"/-");
    if (first == std::wstring::npos) return false;
    flagName = flagName.substr(first);

    std::wstring args(cmdLine);
    size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && iswspace(args[i])) ++i;
        if (i >= args.size()) break;

        const size_t begin = i;
        if (args[i] == L'"') {
            // 引号参数：跳到配对引号之后
            ++i;
            while (i < args.size() && args[i] != L'"') ++i;
            if (i < args.size()) ++i;
        } else {
            while (i < args.size() && !iswspace(args[i])) ++i;
        }

        std::wstring token = args.substr(begin, i - begin);
        const size_t t = token.find_first_not_of(L"/-");
        if (t == std::wstring::npos) continue;
        token = token.substr(t);
        if (_wcsicmp(token.c_str(), flagName.c_str()) == 0) return true;
    }
    return false;
}

static int DoInstall(bool quiet) {
    const std::wstring dir = GetInstallDir();
    if (dir.empty()) {
        if (!quiet) MessageBoxW(nullptr, L"无法确定安装目录。", kProductName, MB_OK | MB_ICONERROR);
        return 1;
    }
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring dllPath = dir + L"\\LG_Hash.dll";
    if (!ExtractDll(dllPath)) {
        // 旧版仍被 explorer 加载时会锁定文件，退回带序号的文件名
        dllPath = UniquePath(dllPath);
        if (!ExtractDll(dllPath)) {
            if (!quiet)
                MessageBoxW(nullptr, L"无法从安装包中提取 DLL 文件。", kProductName, MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    DWORD code = 0;
    if (!RunRegsvr32(dllPath, quiet, &code)) {
        DeleteFileW(dllPath.c_str());
        if (!quiet) {
            wchar_t msg[512];
            swprintf_s(msg, L"注册失败（regsvr32 返回 %lu）。\n\n"
                            L"可手动执行：regsvr32 \"%s\"", code, dllPath.c_str());
            MessageBoxW(nullptr, msg, kProductName, MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    CleanStaleDlls(dir, dllPath);
    RegisterUninstallEntry(dir);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (!quiet) {
        std::wstring msg = L"文件哈希属性页扩展已安装。\n\n";
        msg += L"右键任意文件 → 属性 → 「哈希」页。\n\n";
        msg += L"支持算法：CRC32 / MD5 / SHA-1 / SHA-256 / SHA-512\n";
        msg += L"切到「哈希」页即自动开始计算，无需再点按钮。\n\n";
        msg += L"安装位置：" + dllPath + L"\n";
        msg += L"卸载：设置 → 应用 → 已安装的应用 → LG_Hash";
        MessageBoxW(nullptr, msg.c_str(), kProductName, MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}

static int DoUninstall(bool quiet) {
    const std::wstring dir = GetInstallDir();
    const std::wstring mainDll = dir + L"\\LG_Hash.dll";

    RunRegsvr32(mainDll, true, nullptr);
    CleanStaleDlls(dir, std::wstring()); // keep 为空表示全部清理

    // 兜底：注册可能被升级时改名次数多的旧 DLL（LG_Hash_XXXXXXXX.dll）占用，
    // 此时 mainDll 可能已不存在，regsvr32 也会失败，但 CLSID 树仍留在注册表里。
    // 先判断注册项是否还指向本安装目录的 DLL；是就整个删树，避免卸载后残留。
    {
        wchar_t registered[MAX_PATH * 2] = {};
        DWORD rsz = sizeof(registered);
        const LSTATUS got = RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Classes\\CLSID\\{06FE4DCB-7AD4-4594-83CD-E7753BDA89FC}\\InprocServer32",
            nullptr, RRF_RT_REG_SZ, nullptr, registered, &rsz);
        bool removeTree = false;
        if (got == ERROR_SUCCESS && registered[0]) {
            // 注册值含目录前缀即判定是我们的；含旧版改名序号的文件名一样匹配
            removeTree = StrStrIW(registered, dir.c_str()) != nullptr;
        } else if (got == ERROR_FILE_NOT_FOUND) {
            // 注册项被部分删掉（如手动清理后剩下空壳），也可以删树
            removeTree = true;
        }
        if (removeTree) {
            RegDeleteTreeW(HKEY_CURRENT_USER,
                           L"Software\\Classes\\CLSID\\{06FE4DCB-7AD4-4594-83CD-E7753BDA89FC}");
            RegDeleteTreeW(HKEY_CURRENT_USER,
                           L"Software\\Classes\\*\\shellex\\PropertySheetHandlers"
                           L"\\{06FE4DCB-7AD4-4594-83CD-E7753BDA89FC}");
        }
    }

    DeleteFileW(mainDll.c_str());
    RemoveDirectoryW(dir.c_str());
    RemoveUninstallEntry();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    // 删不掉通常是因为 explorer 还加载着 DLL，必须如实告知而不是报“已卸载”
    const bool stillThere = PathFileExistsW(mainDll.c_str()) != FALSE;
    if (!quiet) {
        if (stillThere) {
            MessageBoxW(nullptr,
                        L"注册信息已移除，但 DLL 仍被资源管理器占用，文件未能删除。\n\n"
                        L"重启 Windows 后再次运行卸载即可清除文件。\n"
                        L"（属性页已不再出现）",
                        kProductName, MB_OK | MB_ICONWARNING);
        } else {
            MessageBoxW(nullptr, L"文件哈希属性页扩展已卸载。\n\n右键文件 → 属性中不再出现「哈希」页。",
                        kProductName, MB_OK | MB_ICONINFORMATION);
        }
    }
    return stillThere ? 2 : 0;
}

// 列出命令行里的未知参数。安装器只接受 /install /uninstall /quiet /s /help，
// 拼错参数（如 /unistall）不该静默变成"安装"。
static std::wstring FirstUnknownArg(LPCWSTR cmdLine) {
    static const wchar_t* known[] = { L"install", L"uninstall", L"quiet", L"s", L"help", L"?" };
    if (!cmdLine) return std::wstring();

    std::wstring args(cmdLine);
    size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && iswspace(args[i])) ++i;
        if (i >= args.size()) break;
        const size_t begin = i;
        if (args[i] == L'"') {
            ++i;
            while (i < args.size() && args[i] != L'"') ++i;
            if (i < args.size()) ++i;
        } else {
            while (i < args.size() && !iswspace(args[i])) ++i;
        }
        std::wstring token = args.substr(begin, i - begin);
        const size_t t = token.find_first_not_of(L"/-");
        if (t == std::wstring::npos) continue;   // 纯空白或纯符号，忽略
        token = token.substr(t);

        bool matched = false;
        for (const wchar_t* k : known) {
            if (_wcsicmp(token.c_str(), k) == 0) { matched = true; break; }
        }
        if (!matched) return token;
    }
    return std::wstring();
}

static void ShowUsage() {
    // 0x000D 0x000A 由码点构造，避免源码中出现跨行的转义字符
    const wchar_t CRLF[3] = { (wchar_t)0x0D, (wchar_t)0x0A, 0 };
    std::wstring msg;
    msg += L"LG_Hash 安装程序";
    msg += CRLF; msg += CRLF;
    msg += L"  LG_Hash.exe                     安装";
    msg += CRLF;
    msg += L"  LG_Hash.exe /quiet              静默安装";
    msg += CRLF;
    msg += L"  LG_Hash.exe /uninstall          卸载";
    msg += CRLF;
    msg += L"  LG_Hash.exe /uninstall /quiet   静默卸载";
    msg += CRLF;
    msg += L"  LG_Hash.exe /help               显示本帮助";
    msg += CRLF; msg += CRLF;
    msg += L"安装到 %LOCALAPPDATA%\\LG_Hash，注册到当前用户。";
    MessageBoxW(nullptr, msg.c_str(), kProductName, MB_OK | MB_ICONINFORMATION);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    const bool quiet = HasFlag(cmdLine, L"/quiet") || HasFlag(cmdLine, L"/s");

    if (HasFlag(cmdLine, L"/help") || HasFlag(cmdLine, L"/?")) {
        // 静默模式下不弹窗，直接返回（用于脚本探测）
        if (!quiet) ShowUsage();
        return 0;
    }

    // 未知参数直接报错退出，避免拼写错误被当成安装执行
    const std::wstring unknown = FirstUnknownArg(cmdLine);
    if (!unknown.empty()) {
        if (!quiet) {
            const wchar_t CRLF2[3] = { (wchar_t)0x0D, (wchar_t)0x0A, 0 };
            std::wstring tip = L"无法识别的参数：" + unknown;
            tip += CRLF2; tip += CRLF2;
            tip += L"运行 LG_Hash.exe /help 查看用法。";
            MessageBoxW(nullptr, tip.c_str(), kProductName, MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    if (HasFlag(cmdLine, L"/uninstall")) return DoUninstall(quiet);
    return DoInstall(quiet);
}
