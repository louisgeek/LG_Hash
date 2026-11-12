#pragma once

#include <string>

// 路径级工具，与 UI 无依赖，可直接被测试程序链接。
namespace PathUtil {

// 两个路径是否指向同一个文件。先按字面对比（大小写不敏感），不同则再各取一次
// 规范化后的最终路径对比，识别短名、相对路径、符号链接等同文件的不同写法。
// 任一为空，或某条路径因权限等原因无法解析时，保守返回 false（不当作同一文件），
// 避免导出时因保护误判而拦截正常另存。
bool SameFile(const std::wstring& a, const std::wstring& b);

} // namespace PathUtil
