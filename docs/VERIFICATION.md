# Verification

Every level below was run; the command that proves it is named. Levels 1-4 need
no hardware. Level 5 is the hardware session, which reached the scanner but not
yet a full scan - see the end of section 5.

## 1. Build

```
tools\build.cmd
```

MSVC 14.44, x86. Produces:

| Artefact | Purpose |
|---|---|
| `build\ScanSnap-iX1300.ds` | the data source (32-bit PE DLL, exports `DS_Entry` @1) |
| `build\selftest.exe` | protocol test, scripted transport |
| `build\mockscan.exe` | full session test, synthetic device |
| `build\twaintest.exe` | TWAIN triplet test, loads the .ds directly |
| `build\dsmtest.exe` | same, but through a real `twaindsm.dll` |
| `build\probe.exe` | hardware probe |

## 2. Protocol, scripted transport

```
build\selftest.exe
```

Replays the iX1600 front/grayscale command stream (the profile the iX1300
reproduces byte for byte) and checks INQUIRY/VPD parsing, the arm sequence,
hopper decoding, feed + TEST UNIT READY, SCAN, pixel-size READ,
read-image-count, image READ, ILI short-read trimming, status resynchronisation
after a short data-in, and the `03/80/03` end-of-batch sense. 23 assertions.

## 3. Session, synthetic device

```
build\mockscan.exe [outdir]
```

A self-answering fake iX1300 walks the whole batch state machine for colour
300 dpi, gray 200 dpi, line art 300 dpi, colour 150 dpi duplex and colour
**600 dpi** (5100x8400, 128 MB per page, which covers the unrestricted-size
path): geometry, row width per bit depth, TWAIN pixel type, page counts and
clean termination. With `outdir` it writes BMPs; those were inspected for polarity
and row order.

## 4. TWAIN, three ways

```
powershell -File tools\run-dsmtest.ps1     # sets up the scratch root and runs dsmtest
build\twaintest.exe build\ScanSnap-iX1300.ds   # direct load, Mock=1
```

`twaintest` performs exactly the capability writes NAPS2 does in
`TwainScanRunner.ConfigureSource` (XFERMECH=memory, INDICATORS=false,
FEEDERENABLED, DUPLEXENABLED, AUTOMATICSENSEMEDIUM, PIXELTYPE, BITDEPTH,
UNITS, XRES/YRES, BRIGHTNESS, CONTRAST), then ENABLEDS, the
XFERREADY/IMAGEINFO/IMAGEMEMXFER/ENDXFER loop, DISABLEDS, CLOSEDS. 134
assertions, 0 failures. Native DIB and BMP file transfer are covered too:
`NATIVEXFER` returns a real `BITMAPINFOHEADER` (biSize 40, 24 bpp, 2550 px
wide) and `IMAGEFILEXFER` writes a valid BMP.

`dsmtest` does the same through the **actual TWAIN DSM shipped with NAPS2**,
proving discovery (`MSG_GETFIRST`/`GETNEXT` finds the source), `MSG_OPENDS`,
capability routing, transfer and close. Verified:

```
ok  DSM OPENDSM                  ok  IMAGEINFO 2550x4200
ok  DSM discovered the source    ok  page 0 32130000 bytes
ok  DSM OPENDS                    ok  page 1 32130000 bytes
ok  DSM ICAP_XFERMECH = MEMORY   ok  pages 2
ok  DSM ENABLEDS                  ok  DISABLEDS
ok  SETUPMEMXFER 262144           ok  CLOSEDS
```

`dsmtest` also runs against the **real** `C:\Windows\twain_32`, which now holds
the data source, with no scratch root and no import patching:

```
dsmtest "C:\Program Files\NAPS2\lib\_win32\twaindsm.dll" "C:\Windows"
ok  DSM OPENDSM rc=0
ok  DSM discovered the iX1300 data source      <- from the real directory
ok  DSM OPENDS rc=0
ok  DSM ICAP_XFERMECH = MEMORY rc=0
```

`ENABLEDS` then fails *in that run only*, because the installed
`ScanSnapTWAIN.ini` has `Mock=0` and the scanner is wedged (section 5); the
scratch-root run, which sets `Mock=1`, passes all 14 assertions. So discovery,
`OPENDS` and capability routing are proven in the real location, and transfer is
proven synthetically.

### What NAPS2 itself does

`NAPS2.Console.exe --listdevices --driver twain` with `TWAINDSM_LOG` set shows
the exact path NAPS2 takes:

```
Application: "NAPS2.Worker"   "NAPS2 - Not Another PDF Scanner" 8.5  TWAIN 2.3
TWAIN 32 Source Manager (Image Acquisition Interface) version 2, 3, 0, 0
DG_CONTROL/DAT_PARENT/MSG_OPENDSM
  We're deliberately skipping this file: C:\WINDOWS\twain_32\wiatwain.ds
DG_CONTROL/DAT_ENTRYPOINT/MSG_GET   -> TWRC_SUCCESS      (DF_APP2 app)
DG_CONTROL/DAT_IDENTITY/MSG_GETFIRST -> TWRC_ENDOFLIST    (no DS installed yet)
```

That is the confirmation that matters: NAPS2 loads the **32-bit** DSM, opens it
with a window handle, requests the 2.x entry point, and enumerates
`%WINDIR%\twain_32`. That enumeration is the one thing that used to be empty;
it is now not, and `msg_getfirst` returns the source above.

## 5. Hardware

The scanner is now attached and bound to `usbscan.sys`
(`USB\VID_04C5&PID_162C\8&2EFE0359&0&7`, `Present = True`, `Class = Image`,
`Service = usbscan`, problem code 0). Its descriptors, read live through
`IOCTL_GET_DEVICE_DESCRIPTOR` / `IOCTL_GET_PIPE_CONFIGURATION`:

| Fact | Observed value |
|---|---|
| Device | `04c5:162c`, bcdDevice `0100` |
| Pipes | exactly two, both bulk: `0x81` IN, `0x02` OUT, 1024-byte packets |
| Channel alignment | 1024 read, 1024 write (`IOCTL_GET_CHANNEL_ALIGN_RQST`) |
| Interrupt / event pipe | none (`EventChannelSize` = 0) |
| Endpoint state | both `GET_STATUS` = `0x0000`, i.e. **not** halted |

That replaces the "pipe numbers are guessed" caveat: the driver's defaults
`pipeIn = 0x81`, `pipeOut = 0x02` are what the unit reports, and the WinUSB
path enumerates the same pair from the interface descriptor.

A real session did reach the scanner once. The 23:30 log is a genuine one:

```
proto: inquiry 'FUJITSU' 'ScanSnap iX1300' rev '0M00' adf 1 flatbed 0
proto: vpd basic 600x600 max 600x600 window 5184x71080 color 1 gray 1 lineart 1 duplex 0
proto: colour interlace 0 accepted
proto: pixel size front 2550x3300
```

So INQUIRY, VPD, the `SET WINDOW` interlace probe and the pixel-size READ all
work against the real unit, and Letter at 300 dpi is 2550x3300 as expected.
Every response above was read from the device, not synthesised.

### The unit is currently wedged

The scan that produced that log was interrupted (the session's machine went
down mid-run), and the unit is now stuck in its own "scanning" state: the
Scan button flashes blue, bulk OUT is accepted, and bulk IN never completes.
Measured on the attached unit:

| Probe | Result |
|---|---|
| `TEST UNIT READY`, 45 s deadline | no bulk-IN completion, three times |
| writes of the 31-byte envelope | complete immediately every time |
| reads of 1024 bytes / 262144 bytes | never complete |
| control transfers (`GET_STATUS`, `GET_DESCRIPTOR`) | succeed |
| endpoint halt state | clear, so not an endpoint stall |
| `IOCTL_CANCEL_IO` + `IOCTL_RESET_PIPE` | succeed, change nothing |
| `CLEAR_FEATURE(ENDPOINT_HALT)`, `SET_INTERFACE 0:0` | succeed, change nothing |
| draining 243 KB of queued data-in | succeeds; the unit then stays silent |

Control transfers still working while bulk-IN never does is the discriminator:
the USB stack and the endpoints are fine, the device firmware is not. No
software sequence reached it, so **it needs a power cycle** (unplug the USB
cable, or close and reopen the ADF cover, which is how this generation powers
on).

The ban on `IOCTL_SET_TIMEOUT` in `src/usb.c` is now explained rather than
guessed: with any non-zero `TimeoutRead` the device aborts every transfer with
`ERROR_OPERATION_ABORTED` (995). All timeouts are user mode.

## What the crash exposed, and what was fixed

Three defects were real, independent of the crash, and are fixed:

1. **A short data-in desynchronised the link permanently.** `do_cmd` read 13
   bytes and gave up if byte 0 was not `0x53`. After the real INQUIRY above,
   the scanner returned a short page (`short page 3113550/25245000 bytes`) and
   the next status read returned `0x08` — image bytes, not an envelope. Every
   command after that failed. `read_status()` now scans forward for a real
   envelope (bounded at 64 reads) so the next command starts aligned.
   Covered by two new `selftest` assertions that start the pipe mid-stream.
2. **`CancelIoEx` halted the endpoint and nothing reset it.** One timeout made
   every later transfer fail instantly with `ERROR_GEN_FAILURE` (31); the old
   log shows exactly this (`read failed err 31` on the attempt after a
   timeout). `sync_io` now resets both usbscan pipes after a cancel, and
   retries once when it sees `ERROR_GEN_FAILURE`.
3. **A dead session was never cleaned up on open.** A unit left mid-batch keeps
   its data-in queued; the next open inherited it and timed out on every
   command. `recover_usbscan()` now aborts outstanding IO, discards the stale
   data-in (243 KB on the real unit), and resets the pipes before the first
   command.

`build\ScanSnapTWAIN.ini` is now written by `tools\build.cmd` with `Mock=1`, so
the test tools are self-contained. `probe.exe` overrides `Mock` and enables
logging in code, because it is the hardware tool.

## 6. Install state

Installed, all verified from this session:

| Step | Result |
|---|---|
| `C:\Windows\twain_32\ScanSnap-iX1300.ds` | byte-identical to `build\` (post-fix) |
| `HKLM\SOFTWARE\WOW6432Node\TWAIN\DataSources` | `ScanSnap-iX1300` = the .ds path |
| Driver package | `oem31.inf` published, "up-to-date on device" |
| Catalog | SHA-256, signed, `signtool verify /pa` clean |
| Certificate | in `LocalMachine\{My,Root,TrustedPublisher}` with a persisted key |

Elevation works non-interactively here: `Start-Process -Verb RunAs` with the
full path to `System32\WindowsPowerShell\v1.0\powershell.exe` returns, so
`install\install.ps1` runs end to end.

### Four bugs in the install scripts

These blocked the install and are fixed:

1. **SDK tool discovery picked the wrong directory.** `Sort-Object Name
   -Descending` over `Windows Kits\10\bin` compares strings, so `x86` sorts
   above `10.0.26100.0` and the script looked for `bin\x86\x64\makecat.exe`.
   It now parses the version and searches each version/arch tree.
2. **`$PSScriptRoot` in a `param` default is empty.** With `[CmdletBinding()]`
   the default is evaluated before the automatic variable is set, so
   `Join-Path $PSScriptRoot ...` threw on every run. Paths are resolved in the
   body now.
3. **`makecat.exe` writes SHA-1 hashes; PnP wants SHA-256.** `pnputil`
   rejected the catalog with "The hash for the file is not present in the
   specified catalog file". The WDK's `inf2cat` is the usual fix, but it is not
   part of the SDK; `New-FileCatalog -CatalogVersion 2` produces the SHA-256
   catalog PnP accepts, and the package then installs.
4. **`signtool /n <subject>` found no certificate.** `-n` matches only a plain
   CN; this subject has a parenthesised value. Selection is by thumbprint
   (`/sha1`) now.

Two related environment traps, both handled: the `Cert:` drive is missing when
a PowerShell 7 directory is in `PSModulePath` (the Security module fails to
load, which also breaks `New-SelfSignedCertificate` and `New-FileCatalog`), so
the script imports the module by full path and uses the .NET store; and the
certificate must be imported with `PersistKeySet`, or `signtool` sees
`HasPrivateKey` but cannot acquire the key.

`install.ps1` also no longer trusts a stale catalog: it regenerates when the
INF's SHA-256 is not inside the existing `.cat`. And `pnputil` exit codes 3010
(reboot required) and 259 (already installed) are treated as success instead of
warning.

What is **not** yet proven on hardware is a complete scan (feed, SCAN, image
READ, duplex), because the unit is wedged; that needs the power cycle above,
then `probe.exe scan out`.

`dsmtest` proves discovery, `OPENDS`, capabilities, transfer and close through
NAPS2's real `twaindsm.dll`, so the TWAIN path itself does not depend on any of
the above.
