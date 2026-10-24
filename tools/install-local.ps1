<#
  Install into a directory the DSM already scans, without admin rights.

  The 2.x DSM enumerates %WINDIR%\twain_32 recursively, so a subdirectory
  there works. That still needs admin; without it, use -Dir with a scratch
  copy of twaindsm.dll (see docs/VERIFICATION.md) or run install\install.ps1
  elevated.
#>
param([string]$Dir = "$env:WINDIR\twain_32")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ds = Join-Path $root 'build\ScanSnap-iX1300.ds'
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
Copy-Item $ds (Join-Path $Dir 'ScanSnap-iX1300.ds') -Force
$ini = Join-Path $Dir 'ScanSnapTWAIN.ini'
if (-not (Test-Path $ini)) {
    Copy-Item (Join-Path $PSScriptRoot 'ScanSnap-iX1300.ini') $ini
}
Write-Host "Installed into $Dir"
