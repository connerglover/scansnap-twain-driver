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
    Write-Host "Binding the scanner to WinUSB ($inf)"
    # Present the INF as a driver package and install it for the device.
    pnputil /add-driver $inf /install
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pnputil returned $LASTEXITCODE. If the device is not plugged in, plug it in and run: pnputil /add-driver `"$inf`" /install"
    }
    $hwid = 'USB\VID_04C5&PID_162C'
    Write-Host "Hardware ID: $hwid"
    Write-Host 'Verify with: Get-PnpDevice -Class USBDevice | Where-Object InstanceId -like "*VID_04C5*"'
}

Write-Host ''
Write-Host 'Done. In NAPS2: Scan > New Profile > Driver: TWAIN, Device: "ScanSnap iX1300".'
