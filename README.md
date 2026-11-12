# HashPage

Windows 资源管理器 Shell 属性页扩展：右键文件 → 属性 → 「哈希」页，
计算 **CRC32 / MD5 / SHA-1 / SHA-256 / SHA-512**，支持粘贴校验、复制、导出 `*.sums`。
多选文件（最多 64 个）串行计算。C++17 / Win32 / 无第三方依赖。

项目主页：<https://github.com/louisgeek/LG_Hash>

## 安装

| 命令 | 作用 |
|---|---|
| `build\HashPage.exe` | 安装到 `%LOCALAPPDATA%\HashPage` 并注册（HKCU，无需管理员） |
| `build\HashPage.exe /quiet` | 静默安装 |
| `build\HashPage.exe /uninstall` | 卸载（也可在「设置 → 应用」或属性页「设置 → 卸载」触发） |
| `register.bat` / `unregister.bat` | 开发期直接注册 / 反注册构建产物 |

升级时自动清理旧 DLL。注册表记录 DLL 绝对路径，移动 DLL 后需重新注册。

## 使用

- **自动计算**：切到「哈希」页即开始；停在其它页不读盘。
- **计算 / 取消**：同一按钮，计算中显示进度，可随时取消。
- **复制**：双击行或「复制选中」（Ctrl+C）；「复制全部」为 `哈希  文件名` 格式。
- **校验**：粘贴外部哈希实时比对，忽略空白、不区分大小写；
  空态不显示结果，命中绿色「匹配」，否则红色「不匹配」。
- **导出**：另存 `*.sums`（UTF-8 无 BOM、CRLF），拒绝写回被哈希的文件，写后强制落盘。
- **多选**：串行计算，用「◀ i/N ▾ ▶」切换查看；导出包含全部已算出的文件。
- **变更检测**：计算前后四层比对（大小 / LastWrite / NTFS ChangeTime / 文件 ID），
  被修改则黄色警告，摘要不可用于完整性校验。

### 设置

点底部「设置」按钮打开对话框（确定生效并持久化到 `HKCU\Software\HashPage`，取消不写回）：

- **算法**：勾选参与计算的算法；未勾选的行直接从列表移除，重勾后值恢复（无需重算）。
- **大写显示**：哈希值大写。
- **卸载…**：触发卸载流程（独立进程）。
- 左下角 `HashPage 1.3.0` 链接打开项目主页。

界面细节：哈希值等宽字体（Consolas）；校验框空态有灰色占位提示；
深色模式跟随系统（可经 `HKCU\Software\HashPage\Theme` 强制 1=浅色 2=深色），
菜单与对话框均自绘适配；语言跟随系统（`LangId` 可指定 2052/1033），
两者修改后需重启 `explorer.exe`。

## 构建

```
build.bat                 :: x64（默认）
build.bat x64 arm64       :: 双架构
```

产物：`build\HashPage.dll`（扩展本体）+ `build\HashPage.exe`（单文件安装器，内嵌 DLL）。
要求 Visual Studio（C++ 工作负载）+ Windows SDK；静态 CRT（`/MT`），目标机无需运行库。
ARM64 需额外安装「MSVC C++ ARM64 生成工具」组件。

## 签名与打包

```
tools\make-dev-cert.ps1   :: 创建自签名开发证书（需管理员，一次）
sign.bat                  :: 签名 build 下所有产物（支持 /pfx /timestamp）
package.bat               :: 校验签名 + 打包到 dist\（含 SHA256SUMS.txt）
```

自签名仅适用于本机开发；公开分发需 CA 证书，否则 SmartScreen 拦截。

## 测试

```
test\run_tests.bat        :: 控制台：算法比对 / 边界 / 取消 / 变更检测（含时间戳还原场景）/ 语言表 / 多选 Job
powershell -File test\verify_tab.ps1        # UI：自动计算、五行哈希、实时校验
powershell -File test\verify_settings.ps1   # UI：设置对话框、动态行、卸载弹窗
powershell -File test\verify_export.ps1     # UI：导出全流程
powershell -File test\verify_change.ps1     # UI：计算期间修改检测
```

其余：`verify_lifecycle`（引用计数）、`verify_upgrade`（升级路径）、`verify_english`（无中文泄漏）、
`verify_export_guard`（导出防覆盖）。UI 测试需要交互式桌面会话。
参考值由 `test/make_reference.py`（Python 标准库）独立生成，与被测实现交叉验证。

## 已知限制

- 对文件夹不生效；计算在 explorer 进程内进行，可随时取消，内存占用与文件大小无关。
- 变更检测在非 NTFS 卷降级为大小 + 时间，不适合对抗性场景。
- 导出防覆盖依赖路径规范化比较，权限不足时退化为仅靠保存对话框确认。
- 公开分发需 CA 证书；ARM64 构建路径未在本机验证；无 MSI 部署包。
- 语言与主题在每个宿主进程内只读取一次，修改后需重启 `explorer.exe`。

## 目录结构

```
src/
  dllmain.cpp      DllMain、IClassFactory、注册 / 反注册
  HashExt.cpp      IShellExtInit（收集选中文件）+ IShellPropSheetExt（加页）
  HashPage.cpp     属性页 UI：渲染、校验、导出、设置对话框、深色自绘
  HashJob.cpp/.h   Job 生命周期 + 工作者线程（多文件串行计算）
  HashEngine.cpp   CNG 多算法单遍流式计算 + CRC32 + 变更检测（纯计算，无 UI）
  Lang.cpp/.h      中英文文案表
  Theme.cpp/.h     深色模式判定
  PathUtil.cpp/.h  SameFile（导出防覆盖）
  installer.cpp    安装器：释放内嵌 DLL、注册、卸载、清理旧版本
test/              控制台测试程序与 UI 验证脚本
```
