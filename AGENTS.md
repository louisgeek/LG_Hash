# AGENTS.md

给在本仓库工作的 AI 代理／新协作者的简要说明。面向用户的文档见 `README.md`，构建与打包流程见 `BUILDING.md`。

## 这是什么

Windows 资源管理器 Shell 属性页扩展（`LG_Hash.dll`）：右键文件 → 属性 → 「哈希」页，
计算 CRC32 / MD5 / SHA-1 / SHA-256 / SHA-512，支持粘贴校验、复制、导出 `*.SUMS.txt`。
C++17，Win32 裸 API，无第三方依赖。构建依赖：Visual Studio（C++ 工作负载）+ Windows SDK。

## 铁律

**这是进程内 Shell 扩展，跑在 `explorer.exe` 里。崩了不是崩自己，是整个桌面。**

- C++ 异常绝不能跨越 shell 回调边界：线程入口、COM 方法、`WM_INITDIALOG` 都要 `catch(...)`；分配用 `nothrow`。
- UI 线程不读文件：计算在后台线程（`PSN_SETACTIVE` 时自动启动）。
- 不调用 uxtheme 未公开序号导出；深色模式靠读注册表判断 + 自己画。
- 所有面向用户的文案必须走 `Lang::Get()`（`verify_english.ps1` 会断言英文模式无中文）。
- 需要读注册表的配置（语言 / 主题）只在 `Lang::Refresh()` / `Theme::Refresh()` 里读，两者都在建页时调用；不要把注册表读取塞进 `Lang::Get()` 这类热路径。

## 常用命令

```bash
build.bat                  # x64 构建（默认），build.bat x64 arm64 双架构
test\run_tests.bat         # 控制台测试（全量）
sign.bat                   # 自签名开发证书签名
package.bat                # 签名校验 + 打包到 dist\
```

完整流程、环境要求与故障排查见 `BUILDING.md`。

UI 测试需要交互式桌面会话（会真的打开属性窗口）：
`test\verify_tab.ps1`、`verify_settings.ps1`、`verify_export.ps1`、`verify_change.ps1`、
`verify_lifecycle.ps1`、`verify_upgrade.ps1`、`verify_english.ps1`、`verify_export_guard.ps1`、
`verify_language_switch.ps1`（改 LangId 后重开属性页应换语言，断言无需重启 explorer）。

## 踩过的坑（改代码前必读）

1. **参考值必须独立生成**：`test/reference.txt` 由 `test/make_reference.py`（Python 标准库）产出，不能用被测代码自证。
2. **`.ps1` 必须带 UTF-8 BOM + CRLF**（PowerShell 5.1 把无 BOM 当 ANSI，中文注释会破坏语法）；`.bat` 纯 ASCII + CRLF。
3. **`installer.rc` 的 `IDR_LG_HASH_DLL` 不能省**：`installer.cpp` 里的 `#define` 资源编译器看不到，漏了 RCDATA 被静默丢弃，安装器提不出 DLL。
4. **ARM64**：`rc.exe` 固定用主机的；内嵌 DLL 路径经 `rc /d LG_HASH_DLL_PATH=` 传入，否则 ARM64 安装器打包 x64 的 DLL。
5. **跨进程点按钮用 `PostMessage(WM_COMMAND)`**，`BM_CLICK` 会等模态对话框返回而卡死。
6. **剪贴板可能不可用**（受限会话持续 `ERROR_ACCESS_DENIED`）：测试优先读 ListView 行文本。
7. **`verify_change.ps1` 会写文件**：先在 `%TEMP%` 复制副本再追加，跑完即删——改脚本时保持该行为。
8. **`CleanStaleDlls` 绝不能调 `regsvr32 /u`**：所有版本共用同一 CLSID，反注册旧版会抹掉新版注册，属性页直接消失。`verify_upgrade.ps1` 断言此点。
9. **设置对话框资源必须用 `g_hModule` 加载**：宿主是 explorer，`GetModuleHandle(nullptr)` 取不到 DLL 里的资源。
10. **校验框占位提示不用 `EM_SETCUEBANNER`**：部分系统构建上该消息返回 0（实测），用「空态提示文字 + 灰色配色」等价实现，见 `SetVerifyCue`。占位标志必须先置再改文本，否则占位会被当输入校验。
11. **版本号只有一个来源：`src\version.h`**。改版本只改那里四个数字，`.rc` 通过 `build\version_gen.h`（`tools\gen-version-header.ps1` 生成）取逗号/字符串形式，`.cpp` 用 `LG_HASH_VERSION_SHORT`。`build.bat` 编译前会生成该头并跑 `tools\check-version.ps1`，任何写回硬编码数字的改动都会让构建失败。
    - **`v` 前缀只放在展示位置**（设置对话框文案、git tag、发布页）。不要加到 `.rc` 的 `FILEVERSION`（4 个 16 位整数，加字符编译失败）、`ProductVersion`、卸载项的 `DisplayVersion`——这些会被升级检测和第三方版本解析器按纯数字处理。
    - 分发包名带版本号：`dist\LG_Hash-<x.y.z>-x64.exe`，版本号从 `build\version_gen.h` 读，`package.bat` 自动拼。
    - **为什么 rc 要单独生成头**：`rc.exe` 不支持 `#` 字符串化运算符，`#define STR(x) #x` 会报 `RC2104`。所以 `"0.0.3.0"` 必须由脚本落成字面量，不能由 rc 自己从数字推导。
    - 新增/重命名版本常量时，同步更新 `tools\check-version.ps1` 的检查规则，不要为了让构建通过而删掉校验。

## 代码结构

```
src/
  dllmain.cpp      DllMain、IClassFactory、注册 / 反注册
  HashExt.cpp      IShellExtInit（收集选中文件，上限 64）+ IShellPropSheetExt（加页）
  LG_Hash.cpp      属性页主流程：窗口消息分发、计算任务编排、校验、完成汇总
  LG_HashDlg.cpp   设置对话框（读写 HKCU）+ 导出（IFileSaveDialog、防覆盖、落盘）
  LG_HashDraw.cpp  列表渲染与全部自绘：列头、按钮、菜单、占位提示、语言套用
  LG_HashInternal.h 上面三个 UI 文件共用的内部声明（PageState / MenuEntry / 导出函数）
  HashJob.cpp/.h   Job 生命周期 + 工作者线程（多文件串行计算）
  HashEngine.cpp   CNG 多算法单遍流式计算 + CRC32 + 变更检测（纯计算，无 UI）
  Lang.cpp/.h      中英文文案表
  Theme.cpp/.h     深色模式判定
  PathUtil.cpp/.h  SameFile（导出防覆盖）
  installer.cpp    安装器：释放内嵌 DLL、注册、卸载、清理旧版本
```

`HashEngine` 与 `HashJob` 不依赖 UI，可被测试程序直接链接（`test/hashtest*.cpp`、`jobtest.cpp`）。
`LG_Hash*.cpp` 三个文件只通过 `LG_HashInternal.h` 互相调用，不直接引用彼此的实现细节；
只在单个文件内使用的辅助函数保持 `static`，不进头文件。新增跨文件函数时，声明加到
`LG_HashInternal.h` 并去掉定义处的 `static`——漏改会得到 LNK2019，改错会得到 C2011 重定义。
测试临时文件一律写 `%TEMP%`，不在仓库根目录留残留；`test/large.bin` 与 `reference.txt` 用脚本重建，不要手工编辑。

## 关键设计

- **单遍读盘**：1 MiB 分块，同一缓冲喂 4 个 CNG 哈希 + CRC32 查表（`0xEDB88320`，与 zlib 一致）。
- **变更检测四层比对**：大小 / LastWrite / NTFS ChangeTime / 文件 ID；后两项取不到（非 NTFS）自动降级，不误报。
- **多选串行**：一次读一个文件，结果按 `paths` 下标存 `results`；列表行 = 当前勾选的算法（未勾选直接移除，重勾后值恢复）。
- **进度消息 64 位打包**（wParam）：低 16 位百分比 / 中 16 位文件下标 / 高 32 位任务序号；`WM_APP_DONE` 带 `Job*` + seq，防止旧线程消息误判。
- **`Job` 引用计数**：对话框与工作线程各持一份，关窗置 `alive=false` 再 Post。
- **设置在独立模态对话框**（`IDD_SETTINGS`）：确定才写回并刷新属性页，取消不写回。
- **静态 CRT（`/MT`）**、`g_locks` 双保险（COM 对象 + `pcRefParent`）。

## 提交前自检

1. `build.bat` 通过，`/W3 /WX` 零警告
2. `test\run_tests.bat` 全过
3. 改了界面或交互：跑相关 `verify_*.ps1`

## 已知限制

- **只发布 x64**。ARM64 构建路径保留在脚本里，但本机缺 MSVC ARM64 组件、从未产出过 ARM64 包，也未做真机验证。ARM64 设备占有率低，暂不纳入发布范围——不要因为看到脚本支持就以为可以直接发。
- 自签名证书仅限本机开发，公开分发需 CA 证书。
- 变更检测在非 NTFS 卷降级为大小 + 时间，不适合对抗性场景。
- 配置（算法 / 大写 / 语言 / 主题）在**每次建页时**重读，改完下次打开属性页即生效，不需重启 `explorer.exe`。`Lang::Get()` 仍是纯原子读、不碰注册表，重读只发生在 `Lang::Refresh()` / `Theme::Refresh()`。
