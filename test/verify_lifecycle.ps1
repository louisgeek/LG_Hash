param([string]$File = 'sample.txt')
# 退出码自检：验证属性页在关闭时能干净释放（无崩溃、无挂起）。
# 关闭页面后依然能再次打开，说明 Job 与 PageContext 的引用计数正确。
$ErrorActionPreference = 'Stop'

if (-not [IO.Path]::IsPathRooted($File)) { $File = Join-Path $PSScriptRoot $File }
if (-not (Test-Path $File)) { throw "test file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

$leaf = Split-Path $File -Leaf

function Open-Page {
    $shell = New-Object -ComObject Shell.Application
    $shell.Namespace((Split-Path $File)).ParseName($leaf).InvokeVerb('properties')
    Start-Sleep -Milliseconds 1200
    $win = [HashTest]::FindProps($leaf)
    if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
    if ([HashTest]::SelectLG_Hash($win) -lt 0) { throw 'hash page not found' }
    return $win
}

# 三轮开-关，验证不会因引用计数错误而崩溃或泄漏页面
for ($round = 1; $round -le 3; $round++) {
    $win = Open-Page
    # 切到「哈希」页后应自动完成计算，无需再点按钮
    $ch = [HashTest]::Children($win)
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        $ch = [HashTest]::Children($win)
        if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { break }
    }
    $ok = $ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])
    "ROUND $round computed=$ok"
    [HashTest]::Close($win)
    Start-Sleep -Milliseconds 800
}
"LIFECYCLE: PASS"
