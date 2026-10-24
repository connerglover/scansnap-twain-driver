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

This

1. copies `ScanSnap-iX1300.ds` to `C:\Windows\twain_32` (plus a default
   `ScanSnapTWAIN.ini`),
2. registers the legacy DSM fallback key
   `HKLM\SOFTWARE\WOW6432Node\TWAIN\DataSources`,
3. installs `install\iX1300-TWAIN.inf`, binding the scanner to WinUSB so the
   driver does not need `usbscan.sys` or PFU's USD.

`-SkipUsb` leaves the existing USB binding alone; the driver then opens
`\\.\UsbscanN` instead, which works while ScanSnap Home is still installed.

## Use with NAPS2

*Scan > New Profile*, **Driver: TWAIN**, **Device: ScanSnap iX1300**.
Paper source Auto/Feeder/Duplex, colour/gray/black-and-white, 150-600 dpi.

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
tools/selftest.c   protocol test, scripted transport
tools/mockscan.c   full session test, synthetic device
tools/probe.c      hardware probe
install/           INF, scripts, default ini
docs/PROTOCOL.md   the reverse-engineered protocol
docs/VERIFICATION.md  what was proven, and how
```

## Licence

MIT (see `LICENSE`). Protocol facts are drawn from sane-backends'
`fujitsu` backend (GPL-2.0-or-later) and holgerkrupp/Scan (MIT); no code was
copied, only command layouts and constants.
