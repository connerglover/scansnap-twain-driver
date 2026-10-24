<#
  Build and sign install\ScanSnap-iX1300.cat.

  A PnP device install on x64 Windows requires a catalog signed by a trusted
  publisher. With no WDK present (no inf2cat) this uses the SDK's makecat plus
  signtool, and a self-signed code-signing certificate that it installs into
  LocalMachine\Root and LocalMachine\TrustedPublisher (so the package is
  trusted on this machine only).

  Run elevated. Pass -Subject to reuse an existing certificate.
#>
[CmdletBinding()]
param(
    [string]$Subject = 'CN=ScanSnap iX1300 TWAIN (self-signed)',
    [string]$Cat = (Join-Path $PSScriptRoot 'ScanSnap-iX1300.cat')
)

$ErrorActionPreference = 'Stop'
$inf = Join-Path $PSScriptRoot 'iX1300-TWAIN.inf'
$cdf = Join-Path $PSScriptRoot 'ScanSnap-iX1300.cdf'

$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Windows SDK not found.' }
$makecat = Join-Path $sdk.FullName 'x64\makecat.exe'
$signtool = Join-Path $sdk.FullName 'x64\signtool.exe'
if (-not (Test-Path $makecat)) { throw "makecat.exe not found in $($sdk.FullName)\x64" }
if (-not (Test-Path $signtool)) { throw "signtool.exe not found in $($sdk.FullName)\x64" }

# A .cdf lists the files that go into the catalog.
@"
[CatalogHeader]
Name=ScanSnap-iX1300.cat
ResultDir=$(Split-Path -Parent $Cat)

[CatalogFiles]
<iX1300-TWAIN.inf>=$inf
"@ | Set-Content -Path $cdf -Encoding ASCII

Write-Host "makecat $cdf"
& $makecat $cdf | Out-Host
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Cat)) { throw 'makecat failed.' }

$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating self-signed certificate '$Subject'"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
        -CertStoreLocation Cert:\LocalMachine\My `
        -KeyUsage DigitalSignature -KeyExportPolicy Exportable `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
    foreach ($store in 'Root', 'TrustedPublisher') {
        $path = "Cert:\LocalMachine\$store"
        if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
        $copy = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cert
        $store2 = New-Object System.Security.Cryptography.X509Certificates.X509Store $store, 'LocalMachine'
        $store2.Open('ReadWrite')
        $store2.Add($copy)
        $store2.Close()
        Write-Host "  added to LocalMachine\$store"
    }
}

Write-Host "Signing $Cat"
& $signtool sign /fd SHA256 /a /sm /s My /n $Subject $Cat | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'signtool sign failed.' }
& $signtool verify /pa /all $Cat | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'signtool verify failed.' }
Write-Host "Signed catalog: $Cat"
