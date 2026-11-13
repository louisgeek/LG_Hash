# 构建与打包

面向从源码编译 LG_Hash 的开发者。面向使用者的说明见 [README.md](README.md)。

发布一个版本固定四步，**必须按顺序手动执行**（`package.bat` 只做签名校验和拷贝，不会替你编译或签名）：

```bat
build.bat                 :: 1. 编译 x64
test\run_tests.bat        :: 2. 测试
sign.bat                  :: 3. 签名
package.bat               :: 4. 打包到 dist\（参数需与第 1 步一致）
```

> **发布范围**：当前只发布 x64。ARM64 构建路径保留在脚本里（`build.bat arm64`），
> 但本机未安装 MSVC ARM64 编译器，从未产出过 ARM64 包，也未做真机验证。
> ARM64 设备占有率低，暂不纳入发布；需要时按下面「环境要求」表里的组件说明补装后再验证。

## 环境要求

| 依赖 | 说明 |
|------|------|
| Windows 10/11 | 编译机与目标机（当前只发布 x64） |
| Visual Studio 2019/2022 | 需勾选「使用 C++ 的桌面开发」工作负载（MSVC + Windows SDK） |
| Windows SDK | 提供 `rc.exe` 与 `signtool.exe`（`signtool` 随 SDK 安装，不在 MSVC 里） |
| MSVC ARM64 生成工具 | **仅 ARM64 需要，当前未安装**：VS Installer → 修改 → 单个组件 → 「MSVC C++ ARM64 生成工具」 |
| Python 3 | 仅 `test\run_tests.bat` 需要（生成/比对参考值），须在 `PATH` 中 |

无 CMake、无 vcpkg、无第三方库；脚本通过 `vswhere.exe` 自行定位 Visual Studio，不需要预开「开发者命令提示」。

## 1. 编译 —— `build.bat`

```bat
build.bat                 :: 只编 x64（默认，也是当前发布形态）
build.bat x64 arm64       :: 双架构（需先装 MSVC ARM64 组件）
build.bat arm64           :: 只编 ARM64（需先装 MSVC ARM64 组件）
```

每个架构内部按三步走，任一步失败立即中止并返回非零退出码：

1. `rc.exe` 编译 `src\LG_Hash.rc` → `LG_Hash.res`
2. `cl.exe` 链接扩展本体 → `LG_Hash.dll`（10 个 `.cpp` 一次编译，`/LD` 出 DLL）
3. `rc.exe` 把上一步的 DLL 以 RCDATA 嵌入 `src\installer.rc`，再链接 → `LG_Hash.exe`（单文件安装器）

产物路径（`build\`、`dist\` 均已 gitignore）：

| 架构 | 扩展 DLL | 安装器 EXE |
|------|----------|-----------|
| x64（当前发布） | `build\LG_Hash.dll` | `build\LG_Hash.exe` |
| ARM64（未启用） | `build\arm64\LG_Hash.dll` | `build\arm64\LG_Hash.exe` |

成功时控制台结尾为 `BUILD OK` 并列出产物；失败为 `BUILD FAILED`。

**版本号只有一个来源：`src\version.h`。改版本只改这一处。**

```c
// src/version.h
#define LG_HASH_VERSION_MAJOR 0
#define LG_HASH_VERSION_MINOR 0
#define LG_HASH_VERSION_PATCH 3
#define LG_HASH_VERSION_BUILD 0
```

上面四个数字决定全部四处显示，不需要手工同步：

| 显示位置 | 取值方式 |
|---------|---------|
| DLL / EXE 文件属性、资源管理器「详细信息」 | `.rc` → `build\version_gen.h` → `FILEVERSION` / `*_Version` |
| 「应用和功能」里的版本 | `installer.cpp` 的 `kVersion` → `LG_HASH_VERSION_SHORT` |
| 设置对话框左下角 `LG_Hash 0.0.3` | `LG_HashDlg.cpp` 的 `kVersionText` → `LG_HASH_VERSION_SHORT` |

`build.bat` 在编译前做两件事：

1. `tools\gen-version-header.ps1` 从 `version.h` 生成 `build\version_gen.h`。
   需要这一步是因为 `rc.exe` 的预处理器不支持 `#` 字符串化运算符，`"0.0.3.0"`
   这种字符串形式无法由 rc 自己从数字推导，必须落成字面量。
2. `tools\check-version.ps1` 校验没有人在 `.rc` 或 `.cpp` 里写回硬编码数字。
   发现硬编码会直接 `BUILD FAILED`，不会等到发布后才发现版本对不上。

单独查看当前版本与来源：

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-version.ps1
```

输出形如：

```
source  : src\version.h
version : 0.0.3.0  (displayed as 0.0.3)
derived : LG_Hash.rc, installer.rc (via build\version_gen.h), kVersionText, kVersion

VERSION CHECK: PASS
```

`build\version_gen.h` 是生成物，已在 `.gitignore` 覆盖的 `build\` 下，不要手工编辑。

## 2. 测试 —— `test\run_tests.bat`

编译并运行 5 个控制台测试（哈希算法、边界用例、导出防覆盖、多语言文案、多选 Job），再用 Python 标准库独立生成参考值并逐项比对。
结尾输出 `ALL CONSOLE TESTS PASSED` / `TESTS FAILED`。

**首次在新克隆的仓库上跑测试，先重建测试样本**：`test\large.bin`（64 MiB）被 gitignore，缺失时 `make_reference.py` 会报 `missing fixture`。

```bat
powershell -ExecutionPolicy Bypass -File test\make_fixtures.ps1   :: 生成 sample.txt + large.bin
test\run_tests.bat
```

UI 级验证需要交互式桌面会话（会真的弹出属性窗口），单独运行：
`test\verify_tab.ps1`、`verify_settings.ps1`、`verify_export.ps1`、`verify_export_guard.ps1`、
`verify_change.ps1`、`verify_lifecycle.ps1`、`verify_upgrade.ps1`、`verify_english.ps1`、
`verify_language_switch.ps1`。

## 3. 签名 —— `sign.bat`

分发前必须签名，`package.bat` 会拒绝未签名的产物。首次使用先创建本地开发证书（**需管理员权限**，写入 `CurrentUser\My` 并把公钥加入 `CurrentUser\Root`，否则 `signtool verify` 会报「链终止于不受信任的根」）：

```bat
powershell -ExecutionPolicy Bypass -File tools\make-dev-cert.ps1
```

脚本把指纹写入 `build\devcert-thumbprint.txt`，之后 `sign.bat` 直接读它，无需参数：

```bat
sign.bat                              :: 用默认开发证书签 x64（存在 arm64 产物时一并签）
sign.bat /thumb <SHA1>                :: 指定证书指纹
sign.bat /pfx <file> [/pw <密码>]     :: 用 PFX 证书（正式发布）
sign.bat /timestamp <RFC3161 URL>     :: 覆盖默认时间戳服务器
```

签名对象：`build\LG_Hash.dll`、`build\LG_Hash.exe`，以及存在的 `build\arm64\*`。摘要与证书哈希均为 SHA-256，默认加 DigiCert RFC3161 时间戳。结尾 `SIGN OK` / `SIGNING FAILED`。

> 自签名证书仅限本机开发：其它机器不信任该证书，公开分发必须换成 CA 签发的证书（用 `/pfx`）。

### 安装器内嵌的是签名前的 DLL

`build.bat` 在第 3 步就把**当时的** `LG_Hash.dll` 原样嵌进安装器。按 `build → sign` 走，EXE 有签名，但安装后落到 `%LOCALAPPDATA%` 的 DLL 是签名前那一份。

如果发布要求 DLL 也带签名，按下面的顺序执行：

```bat
:: 1. 编译出 DLL（EXE 里的副本此时未签名，后面会被替换）
build.bat

:: 2. 先签 DLL
sign.bat

:: 3. 用刚签好的 DLL 重新生成安装器 —— 等价于 build.bat 的第 3 步，但不重链 DLL
"%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe" /nologo /I src /I build /d LG_HASH_DLL_PATH=build\LG_Hash.dll /fo build\installer.res src\installer.rc
cl /nologo /std:c++17 /O2 /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE /MT /D_WINDOWS src\installer.cpp /Fo:build\ /Fe:build\LG_Hash.exe /link /SUBSYSTEM:WINDOWS build\installer.res ole32.lib oleaut32.lib uuid.lib bcrypt.lib comctl32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib advapi32.lib

:: 4. 只签 EXE —— 不要再跑 sign.bat，那会重签 DLL 并让内嵌副本再次失效
::    指纹见 build\devcert-thumbprint.txt
"%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /sha1 <指纹> /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 build\LG_Hash.exe
```

第 3 步需要在已执行 `vcvarsall.bat x64` 的命令行里跑（或直接用「x64 本机工具命令提示」）。不能简单再跑一遍 `build.bat`：它会把 DLL 重新链接，DLL 的签名立刻作废。

## 4. 打包 —— `package.bat`

```bat
package.bat                 :: x64（当前发布形态）
package.bat x64 arm64       :: 双架构（需先编译出 ARM64 产物）
```

流程与注意事项：

1. **先递归删除 `dist\`**，再重新创建 —— `dist\` 下的手工文件会一并消失。
2. 逐架构检查 `build[\<arch>]\LG_Hash.exe` 是否存在，缺失则提示先 `build.bat <arch>` 并失败。
3. 用 `signtool verify /pa` 校验安装器签名；**未签名直接失败**（找不到 `signtool` 时只警告，不阻断）。
4. 拷贝安装器为 `dist\LG_Hash-<version>-x64.exe`（例如 `LG_Hash-0.0.3-x64.exe`），版本号取自 `build\version_gen.h`，把根目录 `README.md` 一并复制。
5. 生成 `dist\SHA256SUMS.txt`（小写 SHA-256，`<hash>  <filename>` 两空格分隔，ASCII 编码）。

`dist\` 只放安装器，扩展 DLL 已内嵌在其中，不需要单独分发。

### 文件名里的版本号与 `v` 前缀

- **文件名带版本号，不带 `v`**：`LG_Hash-0.0.3-x64.exe`。多个包并存时能一眼分清。
- **`v` 前缀只用在「标识版本」的地方**（git tag、发布页标题、changelog）。设置对话框里显示 `LG_Hash v0.0.3`。
- **绝不给这些加 `v`**：`.rc` 的 `FILEVERSION`（4 个 16 位整数，加字符直接编译失败）、
  `ProductVersion`、卸载项的 `DisplayVersion`。这些会被 MSI、升级检测、第三方版本解析器
  按纯数字处理，带 `v` 会导致解析失败或比较错误。

实测依据：把 `FileVersion` 写成 `"v0.0.3"` 后，`rc.exe` 仍能编译，`VS_FIXEDFILEINFO`
里的二进制版本也仍是 `0.0.3.0`——但读取**字符串**字段的第三方工具（如 `Version.Parse`）
会失败。所以只在纯展示位置加 `v`。


## 5. 发布前校验

`signtool` 由 Windows SDK 提供，通常不在 `PATH` 里，先指向本机实际安装的 SDK 版本：

```bat
:: 版本号按本机 %ProgramFiles(x86)%\Windows Kits\10\bin\ 下的实际目录调整
set SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe

:: 签名链
"%SIGNTOOL%" verify /pa dist\LG_Hash-0.0.3-x64.exe

:: 校验和应与 SHA256SUMS.txt 一致
certutil -hashfile dist\LG_Hash-0.0.3-x64.exe SHA256
type dist\SHA256SUMS.txt
```

安装冒烟（无需管理员，装到当前用户）：

```bat
dist\LG_Hash-0.0.3-x64.exe /quiet
:: 重启 explorer.exe 后，右键任意文件 → 属性 → 「哈希」
dist\LG_Hash-0.0.3-x64.exe /uninstall /quiet
```

## 6. 常见失败排查

| 现象 | 原因 / 处理 |
|------|------------|
| `Visual Studio not found` | 未装 VS 或缺少 `vswhere.exe`；重装时勾选 C++ 桌面开发 |
| `[arm64] compiler not found for this target` | 缺「MSVC C++ ARM64 生成工具」。`vcvarsall` 在缺编译器时仍返回 0，脚本按 `Hostx64\arm64\cl.exe` 路径精确探测——不能只看 `where cl`，否则连着编 x64+arm64 时会误用上一个架构的编译器，报一堆 `LNK4272` 机器类型冲突 |
| `[arm64] host rc.exe not found` | 缺 Windows SDK；ARM64 的 `rc.exe` 无法在 x64 主机运行，脚本固定用主机 `rc` |
| `signtool.exe not found` | 缺 Windows SDK 签名工具（`sign.bat` 直接失败，`package.bat` 仅警告） |
| `<src>\LG_Hash.exe is not signed - run sign.bat first` | 编译后忘了签名，补跑 `sign.bat` 再 `package.bat` |
| `build\devcert-thumbprint.txt` 不存在 | 先运行 `tools\make-dev-cert.ps1`，或改用 `/thumb` / `/pfx` |
| 安装后属性页不出现 | 改动过 DLL 路径需重新注册；升级后须重启 `explorer.exe`；参见 `AGENTS.md` 第 8 条（旧版反注册会抹掉公共 CLSID） |

## 7. 开发期快速迭代

不打包、不签名，只验证功能：

```bat
build.bat                 :: 编译 x64
register.bat              :: regsvr32 注册 build\LG_Hash.dll（HKCU，无需管理员）
:: 重启 explorer.exe → 右键测试
unregister.bat            :: 反注册
```

`register.bat` 记录的是 DLL 绝对路径，移动或改名后必须重新注册。仓库根目录的 `build\` 与安装目录 `%LOCALAPPDATA%\LG_Hash` 是两套东西：前者给开发期直接注册，后者由 `LG_Hash.exe` 安装。
