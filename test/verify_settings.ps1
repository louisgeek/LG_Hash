param(
    [string]$File,
    [string]$Shot
)
# 设置回归（1.4：独立设置对话框 + 动态行）：
#   算法勾选、大写显示都在「设置」按钮打开的模态对话框里操作；
#   取消勾选的算法行直接从列表移除（其余行上移），重勾后行回到原位。
#   这里通过 PostMessage(WM_COMMAND) 打开对话框，再在对话框里改勾选并点
#   「确定」，验证属性页即时重渲染、设置持久化、「取消」不写回、版本链接显示 LG_Hash。
# 依赖 verify_tab 的前置条件：build\LG_Hash.dll 已注册到当前用户。
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $File) { $File = Join-Path $PSScriptRoot 'sample.txt' }
if (-not $Shot) { $Shot = Join-Path $Root 'build\shot_settings.png' }
if (-not (Test-Path $File)) { throw "test file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

# 清掉旧设置，保证从“全选 + 小写”的默认状态开始
New-Item -Path 'HKCU:\Software\LG_Hash' -Force | Out-Null
Remove-ItemProperty -Path 'HKCU:\Software\LG_Hash' -Name 'Algos','Upper' -ErrorAction SilentlyContinue

$leaf = Split-Path $File -Leaf
$shell = New-Object -ComObject Shell.Application
$shell.Namespace((Split-Path $File)).ParseName($leaf).InvokeVerb('properties')
Start-Sleep -Milliseconds 1500

$win = [HashTest]::FindProps($leaf)
if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
if ([HashTest]::SelectLG_Hash($win) -lt 0) { throw 'hash page not found' }

# 等自动计算完成（CopyAll 启用为信号）
$ch = @{}
for ($i = 0; $i -lt 60; $i++) {
    $ch = [HashTest]::Children($win)
    if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { break }
    Start-Sleep -Milliseconds 250
}
if (-not ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008]))) { throw 'auto-compute did not finish' }

$listHwnd = $ch[1002]
$md5Lower = [HashTest]::ListItemText($listHwnd, 1, 1)
"DEFAULT MD5 (lower): $md5Lower"
if ($md5Lower -cmatch '[A-F]') { throw 'default should be lowercase' }

function Open-SettingsDialog([IntPtr]$Page) {
    [HashTest]::SendCommand($Page, 1028)   # IDM_SETTINGS
    for ($i = 0; $i -lt 30; $i++) {
        $dlg = [HashTest]::FindDlg('设置')
        if ($dlg -eq [IntPtr]::Zero) { $dlg = [HashTest]::FindDlg('Settings') }
        if ($dlg -ne [IntPtr]::Zero) { return $dlg }
        Start-Sleep -Milliseconds 100
    }
    throw 'settings dialog did not appear'
}

# 1) 取消 MD5：该算法的行应直接从列表移除（其余行上移，值保留）
$dlg = Open-SettingsDialog $win
$dc = [HashTest]::Children($dlg)
"SETTINGS-DIALOG: algo-group=$([HashTest]::GetText($dc[1100])) display-group=$([HashTest]::GetText($dc[1106]))"
[HashTest]::SetCheck($dc[1102], $false)
[HashTest]::SendCommand($dlg, 1)   # IDOK
Start-Sleep -Milliseconds 700

$ch = [HashTest]::Children($win)
$count = [HashTest]::ListItemCount($ch[1002])
$names = @(0..($count-1) | ForEach-Object { [HashTest]::ListItemText($ch[1002], $_, 0) })
"ROWS AFTER UNSELECT MD5: $count [$($names -join ', ')]"
if ($count -ne 4) { throw "expected 4 rows after unselecting MD5, got $count" }
if ($names -contains 'MD5') { throw 'MD5 row should be removed from the list' }
# 原 row2 的 SHA-1 上移为 row1，值保留
$row1 = [HashTest]::ListItemText($ch[1002], 1, 1)
$row0 = [HashTest]::ListItemText($ch[1002], 0, 1)
if ($names[1] -ne 'SHA-1') { throw "row1 should be SHA-1, got $($names[1])" }
if ($row1 -cmatch '[A-F]' -or $row1 -eq [char]0x2014) { throw "SHA-1 value lost: [$row1]" }
if ($row0 -eq [char]0x2014) { throw 'CRC32 row should keep its value' }
"PASS algorithm unselect removes row"

# 2) 重新勾选 MD5：行数恢复 5，MD5 行回到原位且值恢复（无需重算）
$dlg = Open-SettingsDialog $win
$dc = [HashTest]::Children($dlg)
[HashTest]::SetCheck($dc[1102], $true)
[HashTest]::SendCommand($dlg, 1)
Start-Sleep -Milliseconds 700
$ch = [HashTest]::Children($win)
$count = [HashTest]::ListItemCount($ch[1002])
$names = @(0..($count-1) | ForEach-Object { [HashTest]::ListItemText($ch[1002], $_, 0) })
$row1b = [HashTest]::ListItemText($ch[1002], 1, 1)
"ROWS AFTER RESELECT: $count [$($names -join ', ')]"
if ($count -ne 5) { throw "expected 5 rows after reselecting MD5, got $count" }
if ($names[1] -ne 'MD5') { throw "row1 should be MD5 again, got $($names[1])" }
if ($row1b -cne $md5Lower) { throw "re-selected row should restore value, got [$row1b]" }
"PASS algorithm reselect (no recompute)"

# 3) 大写显示：对话框里勾选 → 确定 → MD5 变大写
$dlg = Open-SettingsDialog $win
$dc = [HashTest]::Children($dlg)
[HashTest]::SetCheck($dc[1107], $true)
[HashTest]::SendCommand($dlg, 1)
Start-Sleep -Milliseconds 700
$ch = [HashTest]::Children($win)
$md5Upper = [HashTest]::ListItemText($ch[1002], 1, 1)
"UPPER MD5: $md5Upper"
if ($md5Upper -cne $md5Lower.ToUpper()) { throw "uppercase toggle failed: $md5Upper" }
"PASS uppercase via dialog"

# 4) 取消大写
$dlg = Open-SettingsDialog $win
$dc = [HashTest]::Children($dlg)
[HashTest]::SetCheck($dc[1107], $false)
[HashTest]::SendCommand($dlg, 1)
Start-Sleep -Milliseconds 500
$ch = [HashTest]::Children($win)
$md5Back = [HashTest]::ListItemText($ch[1002], 1, 1)
if ($md5Back -cne $md5Lower) { throw 'lowercase restore failed' }
"PASS lowercase restore"

# 5) 取消不写回：对话框里改动后按「取消」(IDCANCEL=2)，设置应保持
$dlg = Open-SettingsDialog $win
$dc = [HashTest]::Children($dlg)
[HashTest]::SetCheck($dc[1107], $true)
[HashTest]::SendCommand($dlg, 2)   # IDCANCEL
Start-Sleep -Milliseconds 500
$ch = [HashTest]::Children($win)
$md5AfterCancel = [HashTest]::ListItemText($ch[1002], 1, 1)
if ($md5AfterCancel -cne $md5Lower) { throw 'cancel should discard changes' }
"PASS cancel discards changes"

# 6) 设置已持久化到注册表（Algos 全选 = 31，Upper 未勾 = 0/缺省）
$algos = (Get-ItemProperty -Path 'HKCU:\Software\LG_Hash').Algos
"REGISTRY Algos: $algos (expect 0x1F = 31, all selected)"
if ($algos -ne 31) { throw "Algos registry value wrong: $algos" }
"PASS settings persisted"

# 7) 设置对话框左下角的版本链接（IDC_SET_LINK=1108）
$dlg = Open-SettingsDialog $win
$dc = [HashTest]::Children($dlg)
if (-not $dc.ContainsKey(1108)) { throw 'version link missing in settings dialog' }
if ($dc.ContainsKey(1109)) { throw 'uninstall button should be removed from settings dialog' }
"SETTINGS-DIALOG-TAIL: link=[$([HashTest]::GetText($dc[1108]))]"
# 关闭设置对话框（取消）
[HashTest]::SendCommand($dlg, 2)
Start-Sleep -Milliseconds 400

[HashTest]::ParkWindow($win)
[HashTest]::CaptureWindowPrint($win, $Shot) | Out-Null
[HashTest]::Close($win)

# 恢复默认设置，避免影响其他测试
Remove-ItemProperty -Path 'HKCU:\Software\LG_Hash' -Name 'Algos','Upper' -ErrorAction SilentlyContinue

"SETTINGS: PASS"