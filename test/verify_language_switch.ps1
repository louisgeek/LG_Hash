param(
    [string]$File
)
# 语言热切换回归：在同一个 explorer 进程内改 LangId，重开属性页应当换语言。
#
# 覆盖的 bug：Lang 用一次性原子标志做初始化，进程内只读一次注册表，
# 改完 LangId 后不重启 explorer.exe 界面永远是旧语言；而 Theme 每次建页都
# 调 Theme::Refresh()，主题能变、语言不能变，两者行为不一致。
#
# 断言：同一进程内 英->中->英 两次切换都生效，无需重启 explorer。
# 注意：这里刻意不重启 explorer，测的就是"能不能不重启"。
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $File) { $File = Join-Path $PSScriptRoot 'sample.txt' }
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

$leaf = Split-Path $File -Leaf
$shell = New-Object -ComObject Shell.Application

# 标签文字：中英各取一次基准，之后按这两个值判断当前语言
$tabs = @{}
function Get-HashTabText {
    $shell.Namespace((Split-Path $File)).ParseName($leaf).InvokeVerb('properties')
    Start-Sleep -Milliseconds 1400
    $win = [HashTest]::FindProps($leaf)
    if ($win -eq [IntPtr]::Zero) { throw 'properties window not found' }
    $text = $null
    $n = [HashTest]::PageCount($win)
    for ($i = 0; $i -lt $n; $i++) {
        $t = [HashTest]::TabText($win, $i)
        # 「哈希」/「Hash」就是我们的页面标题
        if ($t -eq 'Hash' -or $t -eq [string][char]0x54C8 + [char]0x5E0C) { $text = $t; break }
    }
    [HashTest]::Close($win)
    Start-Sleep -Milliseconds 700
    if (-not $text) { throw 'hash page tab not found' }
    return $text
}

$failures = 0
function Assert-Lang([int]$langId, [string]$expect, [string]$label) {
    Set-ItemProperty -Path $key -Name 'LangId' -Value $langId -Type DWord
    $got = Get-HashTabText
    if ($got -eq $expect) {
        "PASS $label (LangId=$langId -> [$got])"
    } else {
        "FAIL $label (LangId=$langId want [$expect] got [$got])"
        $script:failures++
    }
}

try {
    # 先确认两种语言的基准文字，避免把系统默认语言当成断言目标
    Set-ItemProperty -Path $key -Name 'LangId' -Value 1033 -Type DWord
    $en = Get-HashTabText
    Set-ItemProperty -Path $key -Name 'LangId' -Value 2052 -Type DWord
    $zh = Get-HashTabText
    "BASELINE: en=[$en] zh=[$zh]"
    if ($en -eq $zh) { throw "cannot distinguish languages: both [$en]" }

    # 核心断言：同一进程内来回切换，不重启 explorer
    Assert-Lang 1033 $en 'switch to English (no explorer restart)'
    Assert-Lang 2052 $zh 'switch to Chinese (no explorer restart)'
    Assert-Lang 1033 $en 'switch back to English'

    if ($failures -gt 0) {
        "LANGUAGE-SWITCH: FAIL ($failures assertion(s))"
        exit 1
    }
    "LANGUAGE-SWITCH: PASS"
} finally {
    if ($null -ne $prevLang) {
        Set-ItemProperty -Path $key -Name 'LangId' -Value $prevLang -Type DWord
    } else {
        Remove-ItemProperty -Path $key -Name 'LangId' -ErrorAction SilentlyContinue
    }
    if (-not $hadKey) { Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue }
}
