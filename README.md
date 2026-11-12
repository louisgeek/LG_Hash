# LG_Hash

Windows 资源管理器 Shell 属性页扩展：右键文件 → 属性 → 「哈希」页，计算文件哈希。

---

## 功能

### 哈希计算
- CRC32 / MD5 / SHA-1 / SHA-256 / SHA-512 五种算法
- 1 MiB 分块单遍读盘，同一缓冲喂全部算法
- 切到「哈希」页自动开始计算，停在其它页不读盘

### 校验与复制
- 粘贴外部哈希实时比对：忽略空白、不区分大小写，命中绿色「匹配」，否则红色「不匹配」
- 双击行或「复制选中」（Ctrl+C）；「复制全部」为 `哈希  文件名` 格式

### 导出
- 另存 `*.SUMS.txt`（UTF-8 无 BOM、CRLF）
- 多选时导出包含全部已算出的文件
- 拒绝写回被哈希的文件，写后强制落盘

### 多选
- 最多 64 个文件串行计算，用「◀ i/N ▾ ▶」切换查看
- 未勾选的算法行直接从列表移除，重勾后值恢复（无需重算）

### 变更检测
- 计算前后四层比对：大小 / LastWrite / NTFS ChangeTime / 文件 ID
- 被修改则黄色警告，摘要不可用于完整性校验

### 设置与界面
- 算法勾选、大写显示在「设置」对话框中配置，确定生效、取消不写回
- 深色模式跟随系统，菜单与对话框全部自绘适配
- 简体中文 / English 跟随系统语言（`LangId` 可强制指定）
- 哈希值等宽字体（Consolas）对齐易读

---

## 安装

| 命令 | 作用 |
|------|------|
| `LG_Hash.exe` | 安装到 `%LOCALAPPDATA%\LG_Hash` 并注册（HKCU，无需管理员） |
| `LG_Hash.exe /quiet` | 静默安装 |
| `LG_Hash.exe /uninstall` | 卸载（也可在「设置 → 应用」触发） |
| `register.bat` / `unregister.bat` | 开发期直接注册 / 反注册构建产物 |

---

## 构建

```bat
build.bat                 :: x64（默认）
build.bat x64 arm64       :: 双架构
test\run_tests.bat        :: 控制台测试（全量）
sign.bat                  :: 自签名开发证书签名
package.bat               :: 校验签名 + 打包到 dist\（含 SHA256SUMS.txt）
```

产物：`build\LG_Hash.dll`（扩展本体）+ `build\LG_Hash.exe`（单文件安装器，内嵌 DLL）。
要求 Visual Studio（C++ 工作负载）+ Windows SDK；静态 CRT（`/MT`），目标机无需运行库。
ARM64 需额外安装「MSVC C++ ARM64 生成工具」组件。

---

## 本地文件位置（Windows）

- 设置（算法 / 大写）：`HKCU\Software\LG_Hash`
- 主题覆盖：`HKCU\Software\LG_Hash\Theme`（1=浅色 2=深色）
- 安装目录：`%LOCALAPPDATA%\LG_Hash`

设置、语言、主题修改后需重启 `explorer.exe` 生效。

---

## 技术栈

- C++17 / Win32 裸 API，无第三方依赖
- Windows CNG（BCrypt）多算法哈希
- 哈希计算在后台线程，UI 线程零阻塞
