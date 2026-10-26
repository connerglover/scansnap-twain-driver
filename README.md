# ScanSnap iX1300 TWAIN data source (standalone)

A TWAIN 2.x data source for the Fujitsu/Ricoh **ScanSnap iX1300**
(`04c5:162c`) that talks to the scanner directly over USB. **No ScanSnap
software is required** - not ScanSnap Home, not ScanSnap Manager, not PFU's
`fjtw32.dll` or the `SSiX13-x64.dll` WIA driver.

```
NAPS2 (x64) --> NAPS2.Worker.exe (x86) --> twaindsm.dll
                                              |
                                     ScanSnap-iX1300.ds   <- this project
                                              |
                                        WinUSB (or usbscan.sys)
                                              |
                                    ScanSnap iX1300 (SCSI-over-USB)
```

## Build

```
tools\build.cmd
```

Needs Visual Studio 2022 (Build Tools are enough) with the C++ workload. The
data source is deliberately **32-bit**: NAPS2 runs TWAIN in its Win32 worker
process (`lib\NAPS2.Worker.exe`), and the 32-bit DSM only loads 32-bit sources.

## Install

```
powershell -ExecutionPolicy Bypass -File install\install.ps1
```

Run it **elevated**: it writes `C:\Windows\twain_32`, the machine certificate
stores and the driver store. This

1. copies `ScanSnap-iX1300.ds` to `C:\Windows\twain_32` (plus a default
   `ScanSnapTWAIN.ini`),
2. registers the legacy DSM fallback key
   `HKLM\SOFTWARE\WOW6432Node\TWAIN\DataSources`,
3. builds and signs `install\ScanSnap-iX1300.cat` if it does not already cover
   the INF,
4. installs `install\iX1300-TWAIN.inf`, binding the scanner to WinUSB so the
   driver does not need `usbscan.sys` or PFU's USD.

`-SkipUsb` skips steps 3 and 4, leaving the existing USB binding alone; the
driver then opens `\\.\UsbscanN` instead, which works while ScanSnap Home is
still installed.

### The catalog

PnP will not take an unsigned driver package, and the SDK's `makecat.exe`
produces **SHA-1** member hashes, which current Windows rejects with
"The hash for the file is not present in the specified catalog file".
`install.ps1` therefore calls `New-FileCatalog -CatalogVersion 2` (SHA-256)
instead, and signs the result with a self-signed code-signing certificate that
it puts in `LocalMachine\My`, `Root` and `TrustedPublisher` - so the package is
trusted on this machine only. Both steps are in `install\make-catalog.ps1`,
which also works when the WDK (and therefore `inf2cat.exe`) is not installed.

## Use with NAPS2

*Scan > New Profile*, **Driver: TWAIN**, **Device: ScanSnap iX1300**.
Paper source Auto/Feeder/Duplex, colour/gray/black-and-white, 150-600 dpi.

The paper path follows the scanner's sensors at the start of each batch:
sheets in the U-turn ADF are scanned from there; a sheet in the front
**Return Scan** slot is scanned through the return path when the ADF is
empty. With nothing loaded the driver shows "No paper is loaded" and NAPS2
adds no error of its own.

## Scan button

`build\ScanSnapButton.exe` starts a NAPS2 scan when the scanner's **Scan**
button is pressed. It needs no admin rights:

```
build\ScanSnapButton.exe --install
```

copies itself to `%LOCALAPPDATA%\ScanSnap-iX1300`, writes
`ScanSnapButton.ini` there, adds a logon entry (`HKCU\...\Run`) and starts.
`--stop` stops it, `--uninstall` stops it and removes the logon entry.

On a press it runs NAPS2's own scanner-button entry,
`NAPS2.exe "/StiDevice:ScanSnap iX1300"`. If NAPS2 is open, that window
scans and adds the pages; otherwise NAPS2 starts and scans. NAPS2 uses its
default profile when that profile's device is the ScanSnap iX1300, else the
first profile that is; with none it asks for one. Nothing is saved
automatically - save from NAPS2 as usual. Settings, re-read on every press:

| Key | Default | Meaning |
|---|---|---|
| `Naps2` | `%ProgramW6432%\NAPS2\NAPS2.exe` | |
| `Device` | `ScanSnap iX1300` | NAPS2's device id (the TWAIN source name) |
| `LogEnable` / `LogFile` | 0 / `%TEMP%\ScanSnapButton.log` | |

The scanner has no button event, so the watcher keeps the device open and
polls its status every 200 ms, as ScanSnap Home does. WinUSB allows one open
handle: the data source and `probe` ask the watcher to let go
(`transport_request_device`, two named events) and it stays off the device
for 5 s after each hand-over, retrying every 2 s while someone else holds it.

## Configuration

`C:\Windows\twain_32\ScanSnapTWAIN.ini`:

| Key | Default | Meaning |
|---|---|---|
| `LogEnable` | 0 | append a protocol log |
| `LogFile` | `%TEMP%\ScanSnapTWAIN.log` | log path |
| `Mock` | 0 | use the synthetic scanner (no hardware) |
| `Interlace` | -1 | -1 probe, 0 RGB dot, 1 BGR dot, 2 line |
| `ScannerBuffering` | 0 | scanner-side page buffering |
| `LineartRawPolarity` | 0 | keep the raw 1-bit polarity |

## What it implements

| TWAIN piece | Status |
|---|---|
| `DG_CONTROL`: PARENT, IDENTITY, STATUS, CAPABILITY, EVENT, USERINTERFACE, PENDINGXFERS, SETUPMEMXFER, SETUPFILEXFER, ENTRYPOINT | implemented |
| `DG_IMAGE`: IMAGEINFO, IMAGELAYOUT, IMAGENATIVEXFER, IMAGEMEMXFER, IMAGEFILEXFER | implemented |
| Caps | feeder/duplex, pixel type + bit depth, resolution (range), image layout, brightness/contrast/threshold, compression, xfer mech, physical size, serial, device online |
| Transfers | memory (primary; what NAPS2 uses), native DIB, BMP file |
| Multi-page | ADF walking, MSG_XFERREADY per page, `TW_PENDINGXFERS` correctly 0 on the last page |

## Layout

```
src/ssdrv.h        shared types
src/usb.c          WinUSB transport + usbscan.sys fallback + synthetic device
src/proto.c        Fujitsu SCSI-over-USB command set
src/session.c      batch/page state machine
src/image.c        sample conversion (inversion, de-interleave) + DIB/BMP
src/ds.c           TWAIN data source
src/misc.c         config and logging
src/button.c       Scan-button watcher (ScanSnapButton.exe)
tools/selftest.c   protocol test, scripted transport
tools/run-dsmtest.ps1  run the DSM test without admin rights
tools/mockscan.c   full session test, synthetic device
tools/probe.c      hardware probe: probe info|status|scan
install/           INF, scripts, default ini
docs/PROTOCOL.md   the reverse-engineered protocol
docs/VERIFICATION.md  what was proven, and how
```

## Licence

MIT (see `LICENSE`). Protocol facts are drawn from sane-backends'
`fujitsu` backend (GPL-2.0-or-later) and holgerkrupp/Scan (MIT); no code was
copied, only command layouts and constants.

## Notes on the USB binding

`install\install.ps1 -SkipUsb` keeps whatever binding the machine already has.
Two cases work:

| Binding | Transport used | Needs |
|---|---|---|
| WinUSB (`install\iX1300-TWAIN.inf`) | `WinUsb_*` | the INF installed once |
| `usbscan.sys` (ScanSnap Home's INF) | `\\.\UsbscanN` + `ReadFile`/`WriteFile` | nothing extra |

The data source tries the WinUSB interface GUID first and falls back to
`\\.\Usbscan0..7`, matching the vendor ID/product ID via
`IOCTL_GET_DEVICE_DESCRIPTOR`, so it can be tested on a machine that still has
ScanSnap Home installed without touching the driver binding. In that case stop
`PfuSsMon.exe` first: it holds the device open.

Both transports open overlapped and enforce their own deadlines, so a scanner
that stops answering can never hang the TWAIN thread. Two recovery steps keep a
reused unit usable:

- **On open**, the usbscan path aborts outstanding IO, discards whatever data-in
  a previous session left queued, and resets the pipes. Without this, a scanner
  interrupted mid-batch answers nothing to the next session.
- **After a timeout**, `CancelIoEx` leaves the usbscan endpoint halted, so the
  pipe is reset immediately; otherwise every later transfer fails at once with
  `ERROR_GEN_FAILURE` (31).

If the unit is left mid-scan by a machine crash, it can wedge in firmware: the
Scan button flashes blue, bulk OUT is accepted and bulk IN never completes,
while control transfers still work. No software sequence clears that;
power-cycle the scanner (unplug USB, or close and reopen the ADF cover).
See `docs/VERIFICATION.md` section 5 for the measurements.
