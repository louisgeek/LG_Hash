param(
    [string]$File = 'sample.txt',
    [switch]$KeepOpen
)
# 单页诊断：只打开属性页、打印关键控件文字，不做断言。
# 用于排查 UI 回归（例如某条文案为空、按钮未启用）。
$ErrorActionPreference = 'Stop'

if (-not [IO.Path]::IsPathRooted($File)) { $File = Join-Path $PSScriptRoot $File }
if (-not (Test-Path $File)) { throw "test file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

$leaf = Split-Path $File -Leaf
$shell = New-Object -ComObject Shell.Application
$shell.Namespace((Split-Path $File)).ParseName($leaf).InvokeVerb('properties')
Start-Sleep -Milliseconds 1500

$win = [HashTest]::FindProps($leaf)
if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
$page = [HashTest]::SelectLG_Hash($win)
"PAGE-INDEX: $page"
Start-Sleep -Milliseconds 2500

$ch = [HashTest]::Children($win)
foreach ($id in 1001, 1004, 1006, 1008, 1010) {
    if ($ch.ContainsKey($id)) {
        $txt = [HashTest]::GetText($ch[$id])
        $en = [HashTest]::Enabled($ch[$id])
        "${id}: [$txt] enabled=$en"
    } else {
        "${id}: <missing>"
    }
}

if (-not $KeepOpen) { [HashTest]::Close($win) }
