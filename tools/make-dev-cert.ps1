#Requires -RunAsAdministrator
<#
.SYNOPSIS
    为本地开发创建自签名代码签名证书，并加入「受信任的根证书颁发机构」。

.DESCRIPTION
    仅用于本机开发与测试。自签名证书在其它机器上不会被信任，
    公开分发必须改用 CA 签发的证书（见 README）。

    证书放入 CurrentUser\My，公钥同时写入 CurrentUser\Root，
    这样 signtool verify 才能链到受信任根。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make-dev-cert.ps1
#>
param(
    [string]$Subject = 'CN=LG_Hash Development',
    [int]$Years = 3,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'

if (-not $OutFile) { $OutFile = Join-Path $PSScriptRoot '..\build\devcert-thumbprint.txt' }

# 复用已存在且未过期的证书，避免每次构建都堆积新证书
$existing = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date) -and $_.HasPrivateKey } |
    Select-Object -First 1

if ($existing) {
    $cert = $existing
    Write-Host "reuse existing certificate"
} else {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter (Get-Date).AddYears($Years) `
        -KeyUsage DigitalSignature `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')  # EKU: Code Signing
    Write-Host "created new certificate"
}

# 把公钥加入受信任根，否则 signtool verify 会报「链终止于不受信任的根」
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'CurrentUser')
$store.Open('ReadWrite')
try {
    if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
        $store.Add($cert)
        Write-Host "added to CurrentUser\Root"
    } else {
        Write-Host "already in CurrentUser\Root"
    }
} finally {
    $store.Close()
}

$dir = Split-Path -Parent $OutFile
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
Set-Content -Path $OutFile -Value $cert.Thumbprint -Encoding ASCII -NoNewline

Write-Host ""
Write-Host "subject    : $($cert.Subject)"
Write-Host "thumbprint : $($cert.Thumbprint)"
Write-Host "expires    : $($cert.NotAfter)"
Write-Host "written to : $OutFile"
Write-Host ""
Write-Host "now run: sign.bat"
