param(
    [string]$File,
    [string]$Out
)
$ErrorActionPreference = 'Stop'
if (-not $File) { $File = Join-Path $PSScriptRoot 'sample.txt' }
if (-not $Out) { $Out = Join-Path $env:TEMP 'hashpage_export_test.sums' }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

if (Test-Path $Out) { Remove-Item -LiteralPath $Out -Force }

$leaf = Split-Path $File -Leaf
$shell = New-Object -ComObject Shell.Application
$folder = $shell.Namespace((Split-Path $File))
$folder.ParseName($leaf).InvokeVerb('properties')
Start-Sleep -Milliseconds 1500

$win = [HashTest]::FindProps($leaf)
if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
if ([HashTest]::SelectHashPage($win) -lt 0) { throw 'hash page not found' }

# 切到「哈希」页即自动计算，等完成后再导出（IDC_EXPORT = 1010）
$ch = [HashTest]::Children($win)
$autoOk = $false
for ($i = 0; $i -lt 60; $i++) {
    $ch = [HashTest]::Children($win)
    if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { $autoOk = $true; break }
    Start-Sleep -Milliseconds 250
}
if (-not $autoOk) { throw 'hash page did not auto-compute' }
"AUTO-COMPUTE: $autoOk"

# 用 WM_COMMAND 触发「导出」：BM_CLICK 会同步等待模态对话框返回，
# 从外部进程调用会阻塞，所以改用 PostMessage(WM_COMMAND, IDC_EXPORT)。
[HashTest]::SendCommand($win, 1010)

$save = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $save -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    $save = [HashTest]::FindSaveDialog('sample.txt.sums')
}
if ($save -eq [IntPtr]::Zero) {
    [HashTest]::Close($win)
    throw 'save dialog did not appear'
}
"SAVE-DIALOG-SHOWN: True"

[HashTest]::SetDialogFileName($save, $Out)
Start-Sleep -Milliseconds 500
[HashTest]::AcceptDialog($save)

# 保存后对话框关闭、属性页恢复可用，状态栏应显示“导出成功”
$status = ''
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 200
    if (Test-Path $Out) { break }
}
Start-Sleep -Milliseconds 400
$statusNow = [HashTest]::GetText($ch[1004])
"STATUS-AFTER-EXPORT: $statusNow"
# 2 秒定时器到期后应恢复成“完成，用时 …”
Start-Sleep -Milliseconds 2200
$statusLater = [HashTest]::GetText($ch[1004])
"STATUS-AFTER-TIMER: $statusLater"

[HashTest]::Close($win)

if (-not (Test-Path $Out)) { throw "export produced no file: $Out" }

$bytes = [IO.File]::ReadAllBytes($Out)
"EXPORTED: $Out"
"BYTES: $($bytes.Length)"
"BOM: $($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)"

$text = [Text.Encoding]::UTF8.GetString($bytes)
$lines = @($text -split "`r?`n" | Where-Object { $_.Trim() })
"LINES: $($lines.Count)"
"CRLF: $($bytes -contains 13)"

$ref = @{}
Get-Content (Join-Path $PSScriptRoot 'reference.txt') -Encoding UTF8 | ForEach-Object {
    $p = $_ -split ' '
    if ($p[0] -eq $leaf) { $ref[$p[1]] = $p[2] }
}
$algoOrder = @('CRC32', 'MD5', 'SHA1', 'SHA256', 'SHA512')
$pass = 0
for ($i = 0; $i -lt $algoOrder.Count -and $i -lt $lines.Count; $i++) {
    $parts = $lines[$i] -split '\s+'
    $h = $parts[0]
    $expect = $ref[$algoOrder[$i]]
    $nameOk = ($parts[1] -eq $leaf)
    if ($h -eq $expect -and $nameOk) { "PASS $($algoOrder[$i])"; $pass++ }
    else { "FAIL $($algoOrder[$i]): hash=$h name=$($parts[1]) want=$expect/$leaf" }
}
"SUMMARY: $pass/$($algoOrder.Count) algorithms correct"
