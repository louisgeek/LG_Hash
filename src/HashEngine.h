#pragma once

#include <windows.h>
#include <functional>
#include <string>

inline constexpr int kHashCount = 5;

// 取消时 ComputeFileHashes 返回的哨兵文本，调用方据此区分“取消”与“真错误”
inline constexpr wchar_t kCancelledResult[] = L"已取消";

struct HashAlgo {
    const wchar_t* name;
    int hexChars;
};

inline constexpr HashAlgo kHashAlgos[kHashCount] = {
    { L"CRC32", 8 },
    { L"MD5", 32 },
    { L"SHA-1", 40 },
    { L"SHA-256", 64 },
    { L"SHA-512", 128 },
};

// 计算前后从同一个文件句柄取到的元数据快照。任一字段不同即说明文件在计算
// 期间被改动，此时得到的摘要是“混合内容”的哈希，不具备完整性校验意义，
// 必须提示用户。
//
// 比对四层，从弱到强（见 Snapshot 实现）：
//   大小       —— 内容长度变了必改
//   LastWrite  —— 常规写入会更新，但可被工具/还原手段改回
//   ChangeTime —— NTFS 记录「元数据最后变动」，改动通常伴随它变化，
//                 且用户工具几乎不会伪造它（比 LastWrite 难绕）
//   文件 ID    —— GetFileInformationByHandle 的 nFileIndex{Low,High}，
//                 内容改写不动它，但能捕获「同路径换文件」这类极端情况
struct FileChangeInfo {
    bool changed = false;
    bool valid = false;        // 是否成功取到前后快照
    ULONGLONG sizeBefore = 0;
    ULONGLONG sizeAfter = 0;
    FILETIME writeBefore{};
    FILETIME writeAfter{};
    // 附加比对项：获取失败时为 0 且不参与判定（change 仍按大小+时间判）
    ULONGLONG changeBefore = 0;   // NTFS ChangeTime（100ns 单位）
    ULONGLONG changeAfter = 0;
    ULONGLONG fileIdBefore = 0;   // 卷内文件 ID
    ULONGLONG fileIdAfter = 0;

    bool SizeChanged() const { return sizeBefore != sizeAfter; }
};

// 给超过 MAX_PATH 的路径加长路径前缀（UNC 会转成对应的 UNC 形式）。
// 已带前缀或长度不足 260 时原样返回。
std::wstring LongPath(const std::wstring& path);

// 成功返回空串；取消返回 kCancelledResult；其余为错误描述。
// change 非空时回填“计算期间文件是否被修改”。
// want[i] 为 false 的算法跳过计算，对应 outHex[i] 保持为空串；
// want 为空指针时默认计算全部算法。
std::wstring ComputeFileHashes(const std::wstring& path, HANDLE cancelEvent,
                               const std::function<void(int)>& onProgress,
                               std::wstring outHex[kHashCount],
                               FileChangeInfo* change = nullptr,
                               const bool want[kHashCount] = nullptr);
