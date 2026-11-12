param()
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot 'hashlib.ps1')

$shell = New-Object -ComObject Shell.Application
$dir = Split-Path (Join-Path $PSScriptRoot 'sample.txt')
$shell.Namespace($dir).ParseName('sample.txt').InvokeVerb('properties')
Start-Sleep -Milliseconds 2000

$win = [HashTest]::FindProps('sample.txt')
"WIN: $win"
if ($win -eq [IntPtr]::Zero) { exit 1 }

$page = [HashTest]::SelectHashPage($win)
"PAGE: $page"

$ch = [HashTest]::Children($win)
"IDS: " + (($ch.Keys | Sort-Object) -join ', ')
foreach ($k in ($ch.Keys | Sort-Object)) {
    $t = [HashTest]::GetText($ch[$k])
    "  $k = [$t]"
}
[HashTest]::Close($win)
