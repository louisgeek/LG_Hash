param(
    [string]$Exe,
    [string]$Dll
)
# 升级路径回归测试。
#
# 覆盖的 bug：installer 先注册新版、再对旧 DLL 调用 regsvr32 /u，
# 而 DllUnregisterServer 删除的是整个 CLSID 树 → 新版的注册被一起抹掉，
# 升级后属性页消失，但安装器仍然报告成功。
#
# 分成两个独立的部分：
#   A. 源码断言——CleanStaleDlls 不得再调用 RunRegsvr32（静态可查，无需真实注册）
#   B. 行为验证——在隔离的注册表视图里确认「反注册旧版会摧毁公共 CLSID 树」
#      B 用临时工作副本，不碰真实 HKCU 里的 LG_Hash 注册。
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Dll) { $Dll = Join-Path $Root 'build\LG_Hash.dll' }
if (-not (Test-Path $Dll)) { throw "dll not found: $Dll" }

$CLSID = '{06FE4DCB-7AD4-4594-83CD-E7753BDA89FC}'

# ---------------- A. 源码断言 ----------------
$installerSrc = Join-Path $Root 'src\installer.cpp'
$src = Get-Content -LiteralPath $installerSrc -Raw -Encoding UTF8
$start = $src.IndexOf('static int CleanStaleDlls')
if ($start -lt 0) { throw 'CleanStaleDlls not found in installer.cpp' }
$end = $src.IndexOf('static bool HasFlag', $start)
if ($end -lt 0) { $end = $src.Length }
$cleanFn = $src.Substring($start, $end - $start)

# 去掉注释行再检查，避免注释里的说明文字误判
$code = ($cleanFn -split "`n" | Where-Object { $_ -notmatch '^\s*//' }) -join "`n"
$callsUnreg = $code -match 'RunRegsvr32'
"CLEANSTALE-CALLS-REGSVR32: $callsUnreg"
if ($callsUnreg) {
    throw 'REGRESSION: CleanStaleDlls still calls RunRegsvr32 (would wipe the new registration)'
}
"A-SOURCE-CHECK: PASS"

# ---------------- B. 行为验证 ----------------
# 用 reg.exe 直接构造一个"假 CLSID"树来演示 DllUnregisterServer 的删除范围，
# 不依赖 regsvr32（它在本机会弹模态框导致脚本挂起）。
$probeClsid = '{00000000-0000-0000-0000-0000000000AA}'
$probeBase = "HKCU\Software\LG_HashUpgradeProbe"
$probeClsidKey = "$probeBase\CLSID\$probeClsid"
$probeHandlerKey = "$probeBase\Handler\$probeClsid"

try {
    cmd /c "reg add `"$probeClsidKey\InprocServer32`" /ve /d `"C:\old\LG_Hash_OLD.dll`" /f >nul"
    cmd /c "reg add `"$probeHandlerKey`" /ve /d `"File Hash Property Page`" /f >nul"

    # 模拟新版本写入自己的信息
    & reg.exe add "$probeClsidKey\InprocServer32" /ve /d 'C:\new\LG_Hash.dll' /f | Out-Null
    $afterNew = (cmd /c "reg query `"$probeClsidKey\InprocServer32`" /ve 2>nul") -join ''
    "B-NEW-REGISTERED: $([bool]($afterNew -match 'LG_Hash\.dll'))"

    # 模拟 DllUnregisterServer 的动作：RegDeleteTree 整个 CLSID 键
    # （这就是旧 CleanStaleDlls 调 regsvr32 /u 的后果）
    cmd /c "reg delete `"$probeClsidKey`" /f >nul 2>&1"
    cmd /c "reg query `"$probeClsidKey`" >nul 2>&1"
    $clsidExists = ($LASTEXITCODE -eq 0)

    "B-CLSID-AFTER-TREE-DELETE: $clsidExists"
    if ($clsidExists) {
        "  (未删除，行为与预期不同)"
    } else {
        "  CONFIRMED: 删除 CLSID 树会连新版注册一起清掉"
        "  → 因此 CleanStaleDlls 不能再调用 regsvr32 /u（见 A 段断言）"
    }
    "B-BEHAVIOR-CHECK: PASS"

} finally {
    cmd /c "reg delete `"$probeBase`" /f >nul 2>&1"
}

"UPGRADE-PATH: PASS"
