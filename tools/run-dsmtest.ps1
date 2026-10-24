<#
  Run the TWAIN-DSM integration test without administrator rights.

  The 2.x DSM looks for sources in %WINDIR%\twain_32, which normally needs
  admin to write. This builds a scratch root containing

      <root>\twain_32\ScanSnap-iX1300.ds
      <root>\twain_32\ScanSnapTWAIN.ini     (Mock=1)
      <root>\twain_32\twaindsm.dll          (copy of NAPS2's 32-bit DSM)

  and points dsmtest at it, which redirects the DSM's GetWindowsDirectoryA.
#>
[CmdletBinding()]
param([string]$Root = (Join-Path $env:TEMP 'scanroot'))

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ds = Join-Path $root 'build\ScanSnap-iX1300.ds'
$test = Join-Path $root 'build\dsmtest.exe'
if (-not (Test-Path $ds))   { throw "Build first: $ds missing." }
if (-not (Test-Path $test)) { throw "Build first: $test missing." }

$dsm = 'C:\Program Files\NAPS2\lib\_win32\twaindsm.dll'
if (-not (Test-Path $dsm)) { throw "NAPS2's 32-bit DSM not found at $dsm" }

$dir = Join-Path $Root 'twain_32'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Copy-Item $ds  (Join-Path $dir 'ScanSnap-iX1300.ds') -Force
Copy-Item $dsm (Join-Path $dir 'twaindsm.dll')       -Force
Set-Content -Path (Join-Path $dir 'ScanSnapTWAIN.ini') -Encoding ASCII -Value @'
[Driver]
Mock=1
LogEnable=0
'@

& $test (Join-Path $dir 'twaindsm.dll') $Root
exit $LASTEXITCODE
