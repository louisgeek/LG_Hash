param(
    [string]$File,
    [string]$Out
)
# 导出防护回归：确认扩展拒绝把结果写回被哈希的文件。
#
# 覆盖的 bug：DoExport 用 CREATE_ALWAYS 打开目标，
# 若用户把目标设成源文件，源文件会被直接截断。
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $File) { $File = Join-Path $PSScriptRoot 'sample.txt' }
if (-not (Test-Path $File)) { throw "test file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

# 在临时目录里做，避免任何意外影响仓库文件
# 用很短的目录名：属性窗口标题是 "<文件名> 属性"，
# 工作目录路径过长会让标题在测试的窗口枚举里不好定位。
$work = Join-Path $env:TEMP ("hp_" + [Guid]::NewGuid().ToString('N').Substring(0, 4))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$victim = Join-Path $work 'victim.txt'
Copy-Item -LiteralPath $File -Destination $victim -Force
$originalBytes = (Get-Item $victim).Length
$originalHash = (Get-FileHash $victim -Algorithm SHA256).Hash

try {
    $leaf = Split-Path $victim -Leaf
    $shell = New-Object -ComObject Shell.Application
    $shell.Namespace($work).ParseName($leaf).InvokeVerb('properties')
    Start-Sleep -Milliseconds 1500

    # 属性窗口异步出现，轮询等待（比固定 sleep 可靠）
    $win = [IntPtr]::Zero
    for ($i = 0; $i -lt 30 -and $win -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 300
        $win = [HashTest]::FindProps($leaf)
    }
    if ($win -eq [IntPtr]::Zero) { throw "properties window not found for $leaf" }
    if ([HashTest]::SelectHashPage($win) -lt 0) { throw 'hash page not found' }
    $ch = [HashTest]::Children($win)

    # 切到「哈希」页会自动计算，等算完
    for ($i = 0; $i -lt 60; $i++) {
        $ch = [HashTest]::Children($win)
        if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { break }
        Start-Sleep -Milliseconds 250
    }

    $statusBefore = [HashTest]::GetText($ch[1004])
    "STATUS-BEFORE: $statusBefore"

    # 触发导出，然后在保存对话框里把目标设成源文件本身
    [HashTest]::SendCommand($win, 1010)
    $save = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $save -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 250
        $save = [HashTest]::FindSaveDialog($leaf)
    }
    if ($save -eq [IntPtr]::Zero) {
        [HashTest]::Close($win)
        throw 'save dialog did not appear'
    }
    "SAVE-DIALOG: shown"

    [HashTest]::SetDialogFileName($save, $victim)
    Start-Sleep -Milliseconds 400
    [HashTest]::AcceptDialog($save)
    Start-Sleep -Milliseconds 800

    # 目标已存在 → 系统会先弹「是否替换」，选"是"才能到达我们的防护检查
    $confirm = [HashTest]::FindSaveDialog($leaf)
    if ($confirm -ne [IntPtr]::Zero) {
        [HashTest]::AcceptDialog($confirm)   # 确认替换
        Start-Sleep -Milliseconds 800
        "CONFIRM-ACCEPTED"
    }

    # 对话框交互后重新取控件句柄（旧句柄可能已失效）
    $ch2 = [HashTest]::Children($win)
    $statusAfter = if ($ch2.ContainsKey(1004)) { [HashTest]::GetText($ch2[1004]) } else { '' }
    "STATUS-AFTER: $statusAfter"

    [HashTest]::Close($win)

    $nowBytes = (Get-Item $victim).Length
    $nowHash = (Get-FileHash $victim -Algorithm SHA256).Hash
    "SIZE: $originalBytes -> $nowBytes"
    "HASH-UNCHANGED: $($originalHash -eq $nowHash)"

    if ($originalHash -ne $nowHash) {
        throw 'SOURCE WAS OVERWRITTEN - export guard failed'
    }
    # 主断言：源文件未被改动。
    # 状态栏文案受模态对话框层叠影响，自动化下不稳定，仅作提示。
    if ($statusAfter -match '不能覆盖|Cannot overwrite') {
        "REFUSAL-MESSAGE: shown"
    } else {
        "NOTE: 状态栏未捕获拒绝文案（对话框自动化限制），但源文件完好"
    }
    "EXPORT-GUARD: PASS (source intact)"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
