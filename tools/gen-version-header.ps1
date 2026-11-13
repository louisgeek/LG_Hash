#Requires -Version 5.1
<#
.SYNOPSIS
    从 src\version.h 读取版本号，生成 build\version_gen.h 供 .rc 使用。

.DESCRIPTION
    rc.exe 的预处理器不支持 # 字符串化运算符（`#define STR(x) #x` 会报
    RC2104: undefined keyword or key name），所以 "0.0.3.0" 这种字符串形式
    无法由 rc 自己从数字推导。这个脚本在编译前把数字落成字面量：

        build\version_gen.h
            #define LG_HASH_VERSION_COMMA  0,0,3,0
            #define LG_HASH_VERSION_STRING "0.0.3.0"

    版本号本身仍然只在 src\version.h 里维护，这里不写入任何版本知识。

    build.bat 在调用 rc.exe 之前运行本脚本，把 build\ 加进 rc 的 /I 路径。

.PARAMETER OutputPath
    生成文件路径，默认 build\version_gen.h。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen-version-header.ps1

.NOTES
    退出码：0 = 生成成功，1 = version.h 无法解析或数值超限。
#>
param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

if (-not $OutputPath) {
    $OutputPath = Join-Path $Root 'build\version_gen.h'
}

$headerPath = Join-Path $Root 'src\version.h'
if (-not (Test-Path -LiteralPath $headerPath)) {
    Write-Host "gen-version-header: missing src\version.h" -ForegroundColor Red
    exit 1
}

# src\version.h 带 UTF-8 BOM，显式按 UTF8 读，避免 PS 5.1 猜成 ANSI
$lines = @(Get-Content -LiteralPath $headerPath -Encoding UTF8)

$fields = @{}
foreach ($name in 'MAJOR', 'MINOR', 'PATCH', 'BUILD') {
    $marker = '#define LG_HASH_VERSION_' + $name
    foreach ($line in $lines) {
        $idx = $line.IndexOf($marker, [StringComparison]::Ordinal)
        if ($idx -lt 0) { continue }

        $afterRaw = $line.Substring($idx + $marker.Length)
        # 紧跟字母/数字/下划线说明是更长的宏名，跳过
        if ($afterRaw.Length -gt 0 -and
            ($afterRaw[0] -eq '_' -or [char]::IsLetter($afterRaw[0]) -or [char]::IsDigit($afterRaw[0]))) {
            continue
        }

        # 只取开头的数字
        $digits = ''
        foreach ($ch in $afterRaw.Trim().ToCharArray()) {
            if ([char]::IsDigit($ch)) { $digits += $ch }
            else { break }
        }

        if ($digits -ne '') {
            $value = [int]$digits
            if ($value -lt 0 -or $value -gt 65535) {
                Write-Host "gen-version-header: LG_HASH_VERSION_$name = $value exceeds 65535 (FILEVERSION limit)" -ForegroundColor Red
                exit 1
            }
            $fields[$name] = $value
        }
        break
    }
    if (-not $fields.ContainsKey($name)) {
        Write-Host "gen-version-header: cannot parse LG_HASH_VERSION_$name in src\version.h" -ForegroundColor Red
        exit 1
    }
}

$comma  = '{0},{1},{2},{3}' -f $fields['MAJOR'], $fields['MINOR'], $fields['PATCH'], $fields['BUILD']
$dotted = '{0}.{1}.{2}.{3}' -f $fields['MAJOR'], $fields['MINOR'], $fields['PATCH'], $fields['BUILD']

$dir = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$content = @"
// 自动生成，请勿编辑。来源：src\version.h，生成器：tools\gen-version-header.ps1
// rc.exe 不支持 # 字符串化，所以字符串形式在这里落成字面量。
#pragma once

#define LG_HASH_VERSION_COMMA  $comma
#define LG_HASH_VERSION_STRING "$dotted"
"@

# 生成物给 rc.exe 读，用 CRLF；不加 BOM
$content = $content -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($OutputPath, $content + "`r`n", (New-Object Text.UTF8Encoding($false)))

Write-Host "gen-version-header: $dotted -> $OutputPath"
exit 0
