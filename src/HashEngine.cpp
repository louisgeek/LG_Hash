#include "HashEngine.h"
#include "Lang.h"

#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <vector>

namespace {

std::wstring ToHex(const BYTE* data, DWORD len) {
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring s(len * 2, L'0');
    for (DWORD i = 0; i < len; ++i) {
        s[i * 2] = digits[data[i] >> 4];
        s[i * 2 + 1] = digits[data[i] & 0xF];
    }
    return s;
}

const DWORD* Crc32Table() {
    static const std::array<DWORD, 256> table = [] {
        std::array<DWORD, 256> t{};
        for (DWORD i = 0; i < 256; ++i) {
            DWORD c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table.data();
}

std::wstring FormatWinError(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring text;
    if (n && msg) {
        text.assign(msg, n);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
        LocalFree(msg);
    }
    wchar_t buf[128];
    swprintf(buf, 128, Lang::Get(Lang::Id::ErrWinErrorFmt), code);
    if (text.empty()) return buf;
    wchar_t joined[512];
    swprintf(joined, 512, Lang::Get(Lang::Id::WinErrorFmt), buf, text.c_str());
    return joined;
}


// 从已打开的句柄取大小、写入时间、ChangeTime 与文件 ID。用句柄而不是路径，
// 避免父目录被改名等情况下路径失效导致误判。
struct StateSnapshot {
    ULONGLONG size = 0;
    FILETIME writeTime{};
    ULONGLONG changeTime = 0;    // FILE_BASIC_INFO::ChangeTime，0 = 未取到
    ULONGLONG fileId = 0;        // BY_HANDLE_FILE_INFORMATION 的 nFileIndex，0 = 未取到
};

// GetFileInformationByHandleEx 失败返回 NTSTATUS（负数），不是 Win32 错误码
static bool Snapshot(HANDLE file, StateSnapshot* out) {
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) return false;
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) return false;

    out->size = static_cast<ULONGLONG>(standard.EndOfFile.QuadPart);
    // FILE_BASIC_INFO::LastWriteTime 是 LARGE_INTEGER，拆成 FILETIME 的两半
    out->writeTime.dwLowDateTime = basic.LastWriteTime.LowPart;
    out->writeTime.dwHighDateTime = basic.LastWriteTime.HighPart;
    out->changeTime = static_cast<ULONGLONG>(basic.ChangeTime.QuadPart);

    // 文件 ID 是可选增强项：ReFS 上 128 位 ID 取不全、个别 FS 不支持，
    // 失败不影响 change 判定（仍按大小 + 时间比较）
    BY_HANDLE_FILE_INFORMATION bhi{};
    if (GetFileInformationByHandle(file, &bhi)) {
        out->fileId = (static_cast<ULONGLONG>(bhi.nFileIndexHigh) << 32) |
                      static_cast<ULONGLONG>(bhi.nFileIndexLow);
    }
    return true;
}

static bool SameTime(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

int PercentOf(ULONGLONG done, ULONGLONG total) {
    if (total == 0) return 100;
    if (done >= total) return 100;
    return static_cast<int>(done * 100 / total);
}

} // namespace

std::wstring LongPath(const std::wstring& p) {
    if (p.size() < 260 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring ComputeFileHashes(const std::wstring& path, HANDLE cancelEvent,
                               const std::function<void(int)>& onProgress,
                               std::wstring outHex[kHashCount],
                               FileChangeInfo* change,
                               const bool want[kHashCount]) {
    if (change) *change = FileChangeInfo{};

    // 勾选掩码：want 为空时全部计算
    bool use[kHashCount];
    if (want) {
        for (int i = 0; i < kHashCount; ++i) use[i] = want[i];
    } else {
        for (int i = 0; i < kHashCount; ++i) use[i] = true;
    }
    // 一个都没勾时直接按取消处理：读一遍文件却什么都不算没有意义
    bool any = false;
    for (int i = 0; i < kHashCount; ++i) any = any || use[i];
    if (!any) return kCancelledResult;

    // RAII：任何提前返回都不泄漏 CNG 句柄与文件句柄
    struct Guard {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<BYTE> object;
        DWORD digestLen = 0;
        ~Guard() {
            if (hash) BCryptDestroyHash(hash);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        }
    };

    const wchar_t* algNames[4] = {
        BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM,
        BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA512_ALGORITHM,
    };
    Guard bc[4];

    std::wstring err;
    for (int i = 0; i < 4 && err.empty(); ++i) {
        if (!use[i + 1]) continue;   // 未勾选：不初始化该算法的 CNG 对象
        if (BCryptOpenAlgorithmProvider(&bc[i].alg, algNames[i], nullptr, 0) < 0) {
            err = Lang::Get(Lang::Id::ErrAlgoInit);
            break;
        }
        DWORD objLen = 0, cb = 0;
        BCryptGetProperty(bc[i].alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
        BCryptGetProperty(bc[i].alg, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&bc[i].digestLen), sizeof(DWORD), &cb, 0);
        bc[i].object.resize(objLen);
        if (BCryptCreateHash(bc[i].alg, &bc[i].hash, bc[i].object.data(), objLen,
                             nullptr, 0, 0) < 0) {
            err = Lang::Get(Lang::Id::ErrHashObject);
        }
    }

    struct FileGuard {
        HANDLE h = INVALID_HANDLE_VALUE;
        ~FileGuard() {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileGuard;
    HANDLE& file = fileGuard.h;

    if (err.empty()) {
        // 共享读写删：正在被写入或已打开的文件也能计算
        file = CreateFileW(LongPath(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) err = FormatWinError(GetLastError());
    }

    ULONGLONG size = 0;
    FILETIME writeBefore{};
    if (err.empty()) {
        StateSnapshot before{};
        const bool ok = Snapshot(file, &before);
        size = before.size;
        writeBefore = before.writeTime;
        if (change) {
            change->valid = ok;
            change->sizeBefore = before.size;
            change->writeBefore = before.writeTime;
            change->changeBefore = before.changeTime;
            change->fileIdBefore = before.fileId;
        }
        // 快照失败不阻塞计算，只是无法断言“未被修改”（valid=false）
    }

    ULONGLONG done = 0;
    DWORD crc = 0xFFFFFFFFu;
    bool canceled = false;

    if (err.empty()) {
        const DWORD kChunk = 1u << 20;
        std::vector<BYTE> buf(kChunk);
        const DWORD* table = Crc32Table();
        int lastPct = -1;

        for (;;) {
            if (WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) {
                canceled = true;
                break;
            }
            DWORD got = 0;
            if (!ReadFile(file, buf.data(), kChunk, &got, nullptr)) {
                err = FormatWinError(GetLastError());
                break;
            }
            if (got == 0) break;

            for (int i = 0; i < 4; ++i) {
                if (!bc[i].hash) continue;   // 未勾选的算法没有哈希对象
                if (BCryptHashData(bc[i].hash, buf.data(), got, 0) < 0) {
                    err = Lang::Get(Lang::Id::ErrHashData);
                    break;
                }
            }
            if (!err.empty()) break;

            if (use[0])
                for (DWORD i = 0; i < got; ++i)
                    crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);

            done += got;
            const int pct = PercentOf(done, size);
            if (pct != lastPct) {
                lastPct = pct;
                if (onProgress) onProgress(pct);
            }
        }
    }

    if (err.empty() && !canceled) {
        // 收尾前再取一次快照，判断计算期间是否被改动。
        // 四层比对：大小 / LastWrite / ChangeTime / 文件 ID。
        // ChangeTime 与文件 ID 取不到（0）时自动降级为原来的两层。
        if (change && change->valid) {
            StateSnapshot after{};
            if (Snapshot(file, &after)) {
                change->sizeAfter = after.size;
                change->writeAfter = after.writeTime;
                change->changeAfter = after.changeTime;
                change->fileIdAfter = after.fileId;
                change->changed = (after.size != change->sizeBefore) ||
                                  !SameTime(writeBefore, after.writeTime) ||
                                  (after.changeTime != 0 &&
                                   after.changeTime != change->changeBefore) ||
                                  (after.fileId != 0 &&
                                   after.fileId != change->fileIdBefore);
            } else {
                change->valid = false;
            }
        }

        BYTE digest[64];
        for (int i = 0; i < 4; ++i) {
            if (!bc[i].hash) continue;   // 未勾选：不收尾、不写 outHex
            if (BCryptFinishHash(bc[i].hash, digest, bc[i].digestLen, 0) < 0) {
                err = Lang::Get(Lang::Id::ErrHashData);
                break;
            }
            outHex[i + 1] = ToHex(digest, bc[i].digestLen);
        }
        if (err.empty() && use[0]) {
            crc ^= 0xFFFFFFFFu;
            const BYTE c[4] = {
                static_cast<BYTE>(crc >> 24), static_cast<BYTE>(crc >> 16),
                static_cast<BYTE>(crc >> 8), static_cast<BYTE>(crc),
            };
            outHex[0] = ToHex(c, 4);
        }
    }

    if (canceled) return kCancelledResult;
    return err;
}
