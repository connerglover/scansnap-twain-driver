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
    [string]$Cat
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = Split-Path -Parent $MyInvocation.MyCommand.Path }
# A param default cannot use $PSScriptRoot: with [CmdletBinding()] the default
# is evaluated before the automatic variable is set, so it binds an empty
# string and Join-Path throws.
if (-not $Cat) { $Cat = Join-Path $root 'ScanSnap-iX1300.cat' }
$inf = Join-Path $root 'iX1300-TWAIN.inf'

# Every step below writes machine-wide (LocalMachine stores, twain_32), so fail
# early with a clear message instead of a raw CryptographicException.
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell (certificate store and catalog are machine-wide).'
}

# SDK layout is bin\<version>\<arch>\tool.exe. Sorting the bin directory
# descendants by name is wrong: "x86" and "x64" sort above "10.0.26100.0",
# so name order picks an architecture folder as if it were a version. Parse
# the version, order it numerically, and take the first tree that has the
# tool in any architecture.
function Find-SdkTool([string]$Name) {
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin",
        "${env:ProgramFiles(x86)}\Windows Kits\8.1\bin"
    ) | Where-Object { $_ -and (Test-Path $_) }

    foreach ($root in $roots) {
        $versions = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
            Sort-Object { [version]$_.Name } -Descending
        foreach ($v in $versions) {
            foreach ($arch in 'x64', 'x86', 'arm64') {
                $candidate = Join-Path $v.FullName "$arch\$Name"
                if (Test-Path $candidate) { return $candidate }
            }
        }
        $candidate = Join-Path $root $Name
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

$signtool = Find-SdkTool 'signtool.exe'
if (-not $signtool) { throw 'signtool.exe not found under Windows Kits\10\bin.' }
Write-Host "signtool: $signtool"

# Catalog generation, without the WDK.
#
# makecat.exe writes SHA-1 member hashes, and PnP on current Windows rejects
# such a catalog with "The hash for the file is not present in the specified
# catalog file" - it wants SHA-256. inf2cat.exe (the WDK's tool) is the usual
# answer, but the SDK alone has no equivalent. New-FileCatalog
# -CatalogVersion 2 produces exactly the SHA-256 catalog PnP wants, so use it.
#
# Two things it depends on: the Security module must load (a PowerShell 7
# entry in PSModulePath breaks it under 5.1, which is also why the Cert: drive
# was missing), and the catalog must be generated after the INF is final.
if (-not (Get-Command New-FileCatalog -ErrorAction SilentlyContinue)) {
    $secManifest = Join-Path $env:SystemRoot `
        'System32\WindowsPowerShell\v1.0\Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1'
    if (Test-Path $secManifest) {
        Import-Module $secManifest -ErrorAction SilentlyContinue
    }
}
if (-not (Get-Command New-FileCatalog -ErrorAction SilentlyContinue)) {
    throw 'New-FileCatalog is unavailable; cannot build a SHA-256 catalog.'
}

Write-Host "catalog $Cat"
Remove-Item $Cat -ErrorAction SilentlyContinue
New-FileCatalog -Path $inf -CatalogFilePath $Cat -CatalogVersion 2 | Out-Null
if (-not (Test-Path $Cat)) { throw 'catalog generation failed.' }
Write-Host "  $((Get-Item $Cat).Length) bytes"

# The Cert: drive is not always present (PowerShell 7 module paths can break
# the Security provider's type data in 5.1), so use the .NET store directly.
function Get-StoreCert([string]$StoreName, [string]$MatchSubject) {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store $StoreName, 'LocalMachine'
    try {
        $store.Open('ReadOnly')
        return @($store.Certificates | Where-Object { $_.Subject -eq $MatchSubject })
    } finally {
        $store.Close()
    }
}

function Add-CertToStore([string]$StoreName, $Cert) {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store $StoreName, 'LocalMachine'
    try {
        $store.Open('ReadWrite')
        $store.Add($Cert)
        Write-Host "  added to LocalMachine\$StoreName"
    } finally {
        $store.Close()
    }
}

$cert = Get-StoreCert 'My' $Subject | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating self-signed certificate '$Subject'"
    # New-SelfSignedCertificate needs the Cert: drive too, so build the
    # certificate directly. -CertStoreLocation expects a provider path; the
    # equivalent here is writing the result into the store ourselves.
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        $Subject, $rsa,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $false))
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $false))
    $oids = [System.Security.Cryptography.OidCollection]::new()
    [void]$oids.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($oids, $false))

    $created = $req.CreateSelfSigned([DateTimeOffset]::Now.AddDays(-1), [DateTimeOffset]::Now.AddYears(5))
    # PersistKeySet is the load-bearing flag: without it the imported
    # certificate points at an ephemeral key container that dies with the
    # process, so signtool sees HasPrivateKey but cannot acquire the key.
    $pfx = $created.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, '')
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $pfx, '',
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::MachineKeySet -bor
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::PersistKeySet -bor
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)

    Add-CertToStore 'My' $cert
    foreach ($store in 'Root', 'TrustedPublisher') {
        Add-CertToStore $store $cert
    }
}

Write-Host "Signing $Cat"
# Select by thumbprint, not /n <subject>: signtool's -n matches only a plain
# CN subject and finds nothing when the subject has a parenthesised value.
& $signtool sign /fd SHA256 /sm /s My /sha1 $cert.Thumbprint $Cat | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "signtool sign failed (cert $($cert.Thumbprint))."
}
& $signtool verify /pa /all $Cat | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'signtool verify failed.' }
Write-Host "Signed catalog: $Cat"
