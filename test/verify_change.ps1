param(
    [string]$File,
    [string]$Shot,
    [switch]$KeepCopy
)
# 变更检测的 UI 验证：计算大文件期间持续写入，期望出现「文件在计算期间被修改」提示。
#
# 注意：本脚本会往目标文件尾部追加数据（这正是被测行为）。
# 因此它先把源文件复制成临时副本再操作，绝不修改传入的文件本身。
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Shot) { $Shot = Join-Path $Root 'build\shot_changed.png' }

if (-not $File) { $File = Join-Path $PSScriptRoot 'large.bin' }
if (-not (Test-Path $File)) { throw "source file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

# 在临时目录里造一份副本，算完即弃
$work = Join-Path $env:TEMP ("hashpage_change_" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$target = Join-Path $work (Split-Path $File -Leaf)
Copy-Item -LiteralPath $File -Destination $target -Force
"WORK-COPY: $target ($((Get-Item $target).Length) bytes)"

try {
    $leaf = Split-Path $target -Leaf
    $shell = New-Object -ComObject Shell.Application
    $shell.Namespace($work).ParseName($leaf).InvokeVerb('properties')
    Start-Sleep -Milliseconds 1500

    $win = [HashTest]::FindProps($leaf)
    if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
    if ([HashTest]::SelectHashPage($win) -lt 0) { throw 'hash page not found' }
    $ch = [HashTest]::Children($win)
    "WARN-BEFORE: $([HashTest]::Visible($ch[1012]))"

    # 切到「哈希」页会自动开始计算，不等它算完就往副本尾部追加，
    # 制造“计算期间被修改”。自动启动本身约在页激活后几十毫秒内发生，
    # 因此这里几乎与计算起点同时写入。
    Start-Sleep -Milliseconds 40
    $extra = New-Object byte[] (2MB)
    $fs = [IO.File]::Open($target, 'Append', 'Write', 'ReadWrite')
    try { $fs.Write($extra, 0, $extra.Length) } finally { $fs.Close() }

    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 250
        $ch = [HashTest]::Children($win)
        if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { break }
    }

    $warnVisible = [HashTest]::Visible($ch[1012])
    $warnText = if ($warnVisible) { [HashTest]::GetText($ch[1012]) } else { '' }
    "WARN-AFTER: $warnVisible"
    "WARN-TEXT: $warnText"
    "STATUS: $([HashTest]::GetText($ch[1004]))"

    [HashTest]::ParkWindow($win)
    [HashTest]::CaptureWindowPrint($win, $Shot) | Out-Null
    "SHOT: $Shot"

    [HashTest]::Close($win)

    if (-not $warnVisible) { throw 'change warning did not appear' }
    if ($warnText -notmatch '修改') { throw "warning text unexpected: $warnText" }
    "CHANGE-DETECT: PASS"
} finally {
    if (-not $KeepCopy) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        "kept: $work"
    }
}
