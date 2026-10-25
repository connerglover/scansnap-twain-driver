<#
  Install the standalone ScanSnap iX1300 TWAIN data source.

  Two independent bindings:
    * the data source itself -> C:\Windows\twain_32 (discovered by the DSM)
    * the scanner            -> WinUSB (install/iX1300-TWAIN.inf), so the
                                 driver never needs usbscan.sys or PFU's USD

  Run as administrator. -SkipUsb leaves the current USB binding alone, which
  is useful when ScanSnap Home is still installed and you only want to test
  the data source (it then opens \\.\UsbscanN instead).
#>
[CmdletBinding()]
param(
    [switch]$SkipUsb,
    [string]$TwainDir = "$env:WINDIR\twain_32"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ds = Join-Path $root 'build\ScanSnap-iX1300.ds'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell.'
    }
}

Assert-Admin
if (-not (Test-Path $ds)) { throw "Build first: $ds not found." }

Write-Host "Installing $ds -> $TwainDir"
New-Item -ItemType Directory -Force -Path $TwainDir | Out-Null
Copy-Item $ds (Join-Path $TwainDir 'ScanSnap-iX1300.ds') -Force

$ini = Join-Path $TwainDir 'ScanSnapTWAIN.ini'
if (-not (Test-Path $ini)) {
    Copy-Item (Join-Path $PSScriptRoot 'ScanSnap-iX1300.ini') $ini
    Write-Host "Wrote default configuration $ini"
}

# The DS is a plain (non-DF_DS2) source; the 2.x DSM discovers it by scanning
# twain_32, and the legacy twain_32.dll DSM reads this key.
$key = 'HKLM:\SOFTWARE\WOW6432Node\TWAIN\DataSources'
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
New-ItemProperty -Path $key -Name 'ScanSnap-iX1300' -PropertyType String `
    -Value (Join-Path $TwainDir 'ScanSnap-iX1300.ds') -Force | Out-Null
Write-Host 'Registered DSM fallback key.'

if (-not $SkipUsb) {
    $inf = Join-Path $PSScriptRoot 'iX1300-TWAIN.inf'
    $cat = Join-Path $PSScriptRoot 'ScanSnap-iX1300.cat'

    # A catalog is only valid for the exact INF it was built from; a stale one
    # makes pnputil fail with "The hash for the file is not present in the
    # specified catalog file". Regenerate unless the INF's hash is in there.
    $needCatalog = $true
    if (Test-Path $cat) {
        $infBytes = [IO.File]::ReadAllBytes($inf)
        $infHash = [Security.Cryptography.SHA256]::Create().ComputeHash($infBytes)
        $catBytes = [IO.File]::ReadAllBytes($cat)
        # New-FileCatalog stores the raw member hash, so look for the bytes.
        $needCatalog = $true
        for ($i = 0; $i -le $catBytes.Length - $infHash.Length; $i++) {
            $same = $true
            for ($j = 0; $j -lt $infHash.Length; $j++) {
                if ($catBytes[$i + $j] -ne $infHash[$j]) { $same = $false; break }
            }
            if ($same) { $needCatalog = $false; break }
        }
        if (-not $needCatalog) { Write-Host 'Catalog already covers this INF.' }
    }
    if ($needCatalog) {
        Write-Host 'Building a catalog for this INF (self-signed, this machine only).'
        & (Join-Path $PSScriptRoot 'make-catalog.ps1') | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'make-catalog.ps1 failed.' }
    }

    Write-Host "Binding the scanner to WinUSB ($inf)"
    pnputil /add-driver $inf /install
    switch ($LASTEXITCODE) {
        0 { }
        # 3010 = ERROR_SUCCESS_REBOOT_REQUIRED: the package was added; the
        # reboot is what completes the binding on the present device.
        3010 { Write-Host 'Driver package added; reboot to complete the binding.' }
        # 259 = ERROR_NO_MORE_ITEMS: package already present and up to date.
        259 { Write-Host 'Driver package already installed and current.' }
        default {
            Write-Warning ("pnputil returned $LASTEXITCODE. If the scanner is " +
                           "unplugged, plug it in and rerun: pnputil /add-driver " +
                           "`"$inf`" /install")
        }
    }
    Write-Host ''
    Write-Host 'Verify with:  Get-PnpDevice | Where-Object InstanceId -like "*VID_04C5&PID_162C*"'
    Write-Host 'Expected:     Class "USBDevice", Status "OK"  (not "Image")'
}

Write-Host ''
Write-Host 'Done. In NAPS2: Scan > New Profile > Driver: TWAIN, Device: "ScanSnap iX1300".'
