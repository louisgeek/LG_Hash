param(
    [string]$File,
    [string]$Shot
)
$ErrorActionPreference = 'Stop'

# 默认使用本仓库内的测试文件与 build 目录，避免写死个人路径
$Root = Split-Path -Parent $PSScriptRoot
if (-not $File) { $File = Join-Path $PSScriptRoot 'sample.txt' }
if (-not $Shot) { $Shot = Join-Path $Root 'build\shot_tab.png' }
if (-not (Test-Path $File)) { throw "test file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
. (Join-Path $PSScriptRoot 'hashlib.ps1')

$leaf = Split-Path $File -Leaf
$refPath = Join-Path $PSScriptRoot 'reference.txt'
$ref = @{}
Get-Content $refPath -Encoding UTF8 | ForEach-Object {
    $p = $_ -split ' '
    if ($p[0] -eq $leaf) { $ref[$p[1]] = $p[2] }
}
if ($ref.Count -eq 0) { throw "no reference values for $leaf" }

$shell = New-Object -ComObject Shell.Application
$folder = $shell.Namespace((Split-Path $File))
$folder.ParseName($leaf).InvokeVerb('properties')
Start-Sleep -Milliseconds 1500

$win = [HashTest]::FindProps($leaf)
if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
$page = [HashTest]::SelectLG_Hash($win)
if ($page -lt 0) { throw 'hash page not found among tabs' }
"PAGE INDEX: $page"

# 切到「哈希」页即自动计算，不需要再点按钮；等 CopyAll 从禁用变启用作为完成信号。
$ch = [HashTest]::Children($win)
$autoOk = $false
for ($i = 0; $i -lt 60; $i++) {
    $ch = [HashTest]::Children($win)
    if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { $autoOk = $true; break }
    Start-Sleep -Milliseconds 250
}
if (-not $autoOk) { throw 'hash page did not auto-compute after being activated' }
"AUTO-COMPUTE: $autoOk"
"STATUS: $([HashTest]::GetText($ch[1004]))"
"BUTTON: $([HashTest]::GetText($ch[1009]))"
"EXPORT-ENABLED: $([HashTest]::Enabled($ch[1010]))"
# IDC_CHANGEWARN 在未切到哈希页时不一定已建出——缺失时按“不可见”处理
$warnHwnd = if ($ch.ContainsKey(1012)) { $ch[1012] } else { [IntPtr]::Zero }
"CHANGE-WARN-VISIBLE: $([HashTest]::Visible($warnHwnd))"

# 直接读列表内容（不依赖剪贴板：某些环境下剪贴板被系统长期占用）
$listHwnd = $ch[1002]
$readHashes = @()
for ($i = 0; $i -lt 5; $i++) { $readHashes += [HashTest]::ListItemText($listHwnd, $i, 1) }
"LIST-ROW0: $($readHashes[0])"

# 同时验证「复制全部」确实写入了剪贴板（剪贴板不可用时不视为致命）
$copyOk = $true
try {
    [HashTest]::SendCommand($win, 1008)
    Start-Sleep -Milliseconds 600
    $clip = Get-ClipboardRetry -Attempts 5
} catch {
    $copyOk = $false
    "CLIPBOARD-UNAVAILABLE: 跳过剪贴板比对"
}

# 实时校验：输入正确值应显示匹配，输入错误值应显示不匹配
[HashTest]::SetText($ch[1005], $ref['SHA256'])
Start-Sleep -Milliseconds 500
$matchText = [HashTest]::GetText($ch[1006])
[HashTest]::SetText($ch[1005], 'deadbeef')
Start-Sleep -Milliseconds 500
$mismatchText = [HashTest]::GetText($ch[1006])

# 留在“匹配”状态截图，作为实时校验生效的可见证据
[HashTest]::SetText($ch[1005], $ref['SHA256'])
Start-Sleep -Milliseconds 400
[HashTest]::ParkWindow($win)
Start-Sleep -Milliseconds 500

# 优先用 PrintWindow（不受遮挡影响），失败再退回按窗口矩形截屏
$shots = 0
if ([HashTest]::CaptureWindowPrint($win, $Shot)) { $shots++ }
if ($shots -eq 0 -and [HashTest]::CaptureWindow($win, $Shot)) { $shots++ }
"SHOT: $Shot (ok=$shots)"

[HashTest]::Close($win)

$algoOrder = @('CRC32', 'MD5', 'SHA1', 'SHA256', 'SHA512')
$pass = 0
for ($i = 0; $i -lt $algoOrder.Count; $i++) {
    $h = $readHashes[$i]
    $expect = $ref[$algoOrder[$i]]
    if ($h -eq $expect) { "PASS $($algoOrder[$i])"; $pass++ }
    else { "FAIL $($algoOrder[$i]): got $h want $expect" }
}
"LIST-SUMMARY: $pass/5"

if ($copyOk) {
    $lines = @($clip -split "`r?`n" | Where-Object { $_.Trim() })
    if ($lines.Count -ne 5) { "FAIL clipboard line count: $($lines.Count)" }
    $ref2 = 0
    for ($i = 0; $i -lt $algoOrder.Count -and $i -lt $lines.Count; $i++) {
        $parts = $lines[$i] -split '\s+'
        if ($parts[0] -eq $ref[$algoOrder[$i]] -and $parts[1] -eq $leaf) { $ref2++ }
    }
    "CLIPBOARD-SUMMARY: $ref2/5"
}
"MATCH: $matchText"
"MISMATCH: $mismatchText"
