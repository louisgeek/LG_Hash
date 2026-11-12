param(
    [string]$File,
    [string]$Shot
)
# 英文模式回归：把所有 LangId 切到 1033，打开属性页，
# 断言界面上不出现任何中文字符（除了不可翻译的符号）。
#
# 覆盖的 bug：FormatSize 与 HashEngine 的错误串曾硬编码中文，
# 英文系统上会显示 "64.0 MB (67108864 字节)" 这类中英混杂文本。
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $File) { $File = Join-Path $PSScriptRoot 'sample.txt' }
if (-not $Shot) { $Shot = Join-Path $Root 'build\shot_en.png' }
if (-not (Test-Path $File)) { throw "test file not found: $File" }

Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

$key = 'HKCU:\Software\LG_Hash'
$hadKey = Test-Path $key
$prevLang = $null
if ($hadKey) {
    $prevLang = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).LangId
}
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name 'LangId' -Value 1033 -Type DWord

try {
    $leaf = Split-Path $File -Leaf
    $shell = New-Object -ComObject Shell.Application
    $shell.Namespace((Split-Path $File)).ParseName($leaf).InvokeVerb('properties')
    Start-Sleep -Milliseconds 1500

    $win = [HashTest]::FindProps($leaf)
    if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
    if ([HashTest]::SelectLG_Hash($win) -lt 0) { throw 'hash page not found' }
    $ch = [HashTest]::Children($win)

    # 切到「哈希」页会自动计算，等它算完，让所有文案都渲染出来
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        $ch = [HashTest]::Children($win)
        if ($ch.ContainsKey(1008) -and [HashTest]::Enabled($ch[1008])) { break }
    }

    $texts = [ordered]@{
        FileInfo  = [HashTest]::GetText($ch[1001])
        Status    = [HashTest]::GetText($ch[1004])
        VerifyGrp = [HashTest]::GetText($ch[1011])
        CopySel   = [HashTest]::GetText($ch[1007])
        CopyAll   = [HashTest]::GetText($ch[1008])
        Export    = [HashTest]::GetText($ch[1010])
        Button    = [HashTest]::GetText($ch[1009])
    }
    foreach ($k in $texts.Keys) { "$k = [$($texts[$k])]" }

    # 中文检测：CJK 统一表意文字
    $cjk = [regex]'[\u4e00-\u9fff]'
    $bad = @()
    foreach ($k in $texts.Keys) {
        if ($cjk.IsMatch($texts[$k])) { $bad += "$k=[$($texts[$k])]" }
    }

    # 校验提示也要是英文
    [HashTest]::SetText($ch[1005], 'deadbeef')
    Start-Sleep -Milliseconds 500
    $mismatch = [HashTest]::GetText($ch[1006])
    "MismatchText = [$mismatch]"
    if ($cjk.IsMatch($mismatch)) { $bad += "Mismatch=[$mismatch]" }

    [HashTest]::ParkWindow($win)
    [HashTest]::CaptureWindowPrint($win, $Shot) | Out-Null
    [HashTest]::Close($win)

    if ($bad.Count -gt 0) {
        "CJK-FOUND:"
        $bad | ForEach-Object { "  $_" }
        throw 'english mode still contains Chinese text'
    }
    "ENGLISH-MODE: PASS (no CJK characters)"
} finally {
    # 恢复语言设置
    if ($null -ne $prevLang) {
        Set-ItemProperty -Path $key -Name 'LangId' -Value $prevLang -Type DWord
    } else {
        Remove-ItemProperty -Path $key -Name 'LangId' -ErrorAction SilentlyContinue
    }
    if (-not $hadKey) { Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue }
}
