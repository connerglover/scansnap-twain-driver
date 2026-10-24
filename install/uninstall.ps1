<#
  Remove the standalone ScanSnap iX1300 TWAIN data source.
  -RestorePfu puts the scanner back on the PFU binding (usbscan.sys + USD),
  which requires ScanSnap Home to still be installed.
#>
[CmdletBinding()]
param(
    [switch]$RestorePfu,
    [string]$TwainDir = "$env:WINDIR\twain_32"
)

$ErrorActionPreference = 'Stop'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$p = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell.'
}

$ds = Join-Path $TwainDir 'ScanSnap-iX1300.ds'
if (Test-Path $ds) { Remove-Item $ds -Force; Write-Host "Removed $ds" }

$key = 'HKLM:\SOFTWARE\WOW6432Node\TWAIN\DataSources'
if (Test-Path $key) {
    Remove-ItemProperty -Path $key -Name 'ScanSnap-iX1300' -ErrorAction SilentlyContinue
}

# Drop our driver package from the store (ignored when never installed).
$infName = 'iX1300-TWAIN.inf'
$published = (& pnputil /enum-drivers) 2>$null | Out-String
if ($published -match [regex]::Escape($infName)) {
    pnputil /delete-driver $infName /uninstall /force
}

if ($RestorePfu) {
    $oem = Get-ChildItem "$env:WINDIR\INF\oem*.inf" -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content $_.FullName -Raw) -match 'VID_04C5&PID_162C' } |
        Select-Object -First 1
    if ($oem) {
        Write-Host "Reinstalling PFU driver $($oem.Name)"
        pnputil /add-driver $oem.FullName /install
    } else {
        Write-Warning 'No PFU INF matching the iX1300 found; reinstall ScanSnap Home to restore it.'
    }
}
Write-Host 'Done.'
