#Requires -Version 5.1
<#
.SYNOPSIS
    校验版本号只有一个来源（src\version.h），其它地方没有硬编码。

.DESCRIPTION
    版本号唯一来源：src\version.h 的四个数字

        LG_HASH_VERSION_MAJOR / MINOR / PATCH / BUILD

    派生方式：

        .rc 文件      #include "version_gen.h"（tools\gen-version-header.ps1 生成），
                      使用 LG_HASH_VERSION_COMMA / LG_HASH_VERSION_STRING
        kVersionText  LG_HASH_VERSION_SHORT   (src\LG_HashDlg.cpp)
        kVersion      LG_HASH_VERSION_SHORT   (src\installer.cpp)

    这个脚本防止有人把版本号写回硬编码——那种改动能编译通过，
    但会让某处版本永久停在旧值，只有发布后才发现。

.PARAMETER Quiet
    只在失败时输出。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-version.ps1

.NOTES
    退出码：0 = 单一来源生效，1 = 存在硬编码或无法解析。
#>
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$problems = New-Object System.Collections.ArrayList

function Add-Problem {
    param([string]$Message)
    [void]$problems.Add($Message)
}

function Get-Lines {
    param([string]$RelativePath)

    $full = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $full)) {
        Add-Problem "missing file: $RelativePath"
        return @()
    }
    # 源码带 UTF-8 BOM，显式按 UTF8 读，避免 PS 5.1 猜成 ANSI
    return @(Get-Content -LiteralPath $full -Encoding UTF8)
}

function Get-LeadingNumber {
    param([string]$Text)
    $digits = ''
    foreach ($ch in $Text.ToCharArray()) {
        if ([char]::IsDigit($ch)) { $digits += $ch }
        else { break }
    }
    return $digits
}

# ---- 1. 解析 version.h 的四个数字 ----
$versionLines = Get-Lines 'src\version.h'
$fields = @{}

foreach ($name in 'MAJOR', 'MINOR', 'PATCH', 'BUILD') {
    $marker = '#define LG_HASH_VERSION_' + $name
    foreach ($line in $versionLines) {
        $idx = $line.IndexOf($marker, [StringComparison]::Ordinal)
        if ($idx -lt 0) { continue }

        $afterRaw = $line.Substring($idx + $marker.Length)
        # 紧跟字母/数字/下划线说明是更长的宏名，跳过
        if ($afterRaw.Length -gt 0 -and
            ($afterRaw[0] -eq '_' -or [char]::IsLetter($afterRaw[0]) -or [char]::IsDigit($afterRaw[0]))) {
            continue
        }

        $digits = Get-LeadingNumber $afterRaw.Trim()
        if ($digits -eq '') {
            Add-Problem "src\version.h: $marker has no leading number"
        }
        elseif ([int]$digits -gt 65535) {
            Add-Problem "src\version.h: $marker = $digits exceeds 65535 (FILEVERSION limit)"
        }
        else {
            $fields[$name] = [int]$digits
        }
        break
    }
}

$version = $null
$short = $null
if ($fields.Count -eq 4) {
    $version = '{0}.{1}.{2}.{3}' -f $fields['MAJOR'], $fields['MINOR'], $fields['PATCH'], $fields['BUILD']
    $short = '{0}.{1}.{2}' -f $fields['MAJOR'], $fields['MINOR'], $fields['PATCH']
}
else {
    $missing = @('MAJOR', 'MINOR', 'PATCH', 'BUILD') | Where-Object { -not $fields.ContainsKey($_) }
    Add-Problem "src\version.h: missing or unparsable LG_HASH_VERSION_$($missing -join ', LG_HASH_VERSION_')"
}

# ---- 2. .rc 里不得再有硬编码数字，且必须用生成头 ----
foreach ($rel in 'src\LG_Hash.rc', 'src\installer.rc') {
    $lines = Get-Lines $rel
    if ($lines.Count -eq 0) { continue }

    $usesGenerated = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line.Contains('#include "version_gen.h"')) { $usesGenerated = $true }

        $trimmed = $line.Trim()
        $isFileVersion = $trimmed.StartsWith('FILEVERSION', [StringComparison]::OrdinalIgnoreCase) -or
                         $trimmed.StartsWith('PRODUCTVERSION', [StringComparison]::OrdinalIgnoreCase)
        $isValueField  = $trimmed.StartsWith('VALUE', [StringComparison]::OrdinalIgnoreCase) -and
                         ($trimmed.Contains('"FileVersion"') -or $trimmed.Contains('"ProductVersion"'))

        if (-not ($isFileVersion -or $isValueField)) { continue }

        $expectMacro = if ($isValueField) { 'LG_HASH_VERSION_STRING' } else { 'LG_HASH_VERSION_COMMA' }
        if (-not $trimmed.Contains($expectMacro)) {
            Add-Problem "$rel`:$($i + 1) must use $expectMacro from version_gen.h; found: $trimmed"
        }
    }

    if (-not $usesGenerated) {
        Add-Problem "$rel does not include version_gen.h; the version macros would be undefined"
    }
}

# ---- 3. .cpp 的版本常量必须引用宏 ----
$cppChecks = @(
    @{ File = 'src\LG_HashDlg.cpp'; Name = 'kVersionText'; Expect = 'LG_HASH_VERSION_SHORT' }
    @{ File = 'src\installer.cpp';  Name = 'kVersion';      Expect = 'LG_HASH_VERSION_SHORT' }
)

foreach ($check in $cppChecks) {
    $lines = Get-Lines $check.File
    if ($lines.Count -eq 0) { continue }

    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line.TrimStart().StartsWith('//')) { continue }
        if (-not $line.Contains($check.Name)) { continue }

        # kVersionText: static constexpr wchar_t kVersionText[] = ...;
        # kVersion:     #define kVersion ...
        $looksLikeDefinition = $line.Contains($check.Name + '[') -or
                               $line.Contains('#define ' + $check.Name) -or
                               $line.Contains('#define  ' + $check.Name)
        if (-not $looksLikeDefinition) { continue }

        $found = $true
        if (-not $line.Contains($check.Expect)) {
            Add-Problem "$($check.File):$($i + 1) $($check.Name) must reference $($check.Expect); found: $($line.Trim())"
        }
        break
    }

    if (-not $found) {
        Add-Problem "$($check.File): cannot find the definition of $($check.Name)"
    }

    $joined = $lines -join "`n"
    if (-not $joined.Contains('#include "version.h"')) {
        Add-Problem "$($check.File) does not #include `"version.h`""
    }
}

# ---- 4. 生成物若存在，必须与 version.h 一致 ----
$genPath = Join-Path $Root 'build\version_gen.h'
if (Test-Path -LiteralPath $genPath) {
    $genLines = @(Get-Content -LiteralPath $genPath -Encoding UTF8)
    $expectComma  = '{0},{1},{2},{3}' -f $fields['MAJOR'], $fields['MINOR'], $fields['PATCH'], $fields['BUILD']
    $expectString = '"' + $version + '"'

    $commaOk = $false; $stringOk = $false
    foreach ($line in $genLines) {
        if ($line.Contains('LG_HASH_VERSION_COMMA') -and $line.Contains($expectComma)) { $commaOk = $true }
        if ($line.Contains('LG_HASH_VERSION_STRING') -and $line.Contains($expectString)) { $stringOk = $true }
    }
    if ($fields.Count -eq 4 -and -not $commaOk) {
        Add-Problem "build\version_gen.h is stale (LG_HASH_VERSION_COMMA != $expectComma); rerun build.bat"
    }
    if ($fields.Count -eq 4 -and -not $stringOk) {
        Add-Problem "build\version_gen.h is stale (LG_HASH_VERSION_STRING != $expectString); rerun build.bat"
    }
}

# ---- 输出 ----
if ($problems.Count -gt 0) {
    Write-Host ""
    Write-Host "VERSION CHECK: FAIL - version numbers are not single-sourced" -ForegroundColor Red
    Write-Host ""
    foreach ($p in $problems) {
        Write-Host "  $p" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  To change the version, edit only src\version.h:"
    Write-Host "    LG_HASH_VERSION_MAJOR / MINOR / PATCH / BUILD"
    Write-Host ""
    Write-Host "  Do not write version digits into .rc or .cpp files."
    Write-Host "  If you intentionally restructured these declarations, update"
    Write-Host "  tools\check-version.ps1 - do not delete the check to make the build pass."
    Write-Host ""
    exit 1
}

if (-not $Quiet) {
    "source  : src\version.h"
    "version : $version  (displayed as $short)"
    "derived : LG_Hash.rc, installer.rc (via build\version_gen.h), kVersionText, kVersion"
    ""
    "VERSION CHECK: PASS"
}

exit 0
