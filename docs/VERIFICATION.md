# Verification

Every level below was run; the command that proves it is named. Levels 1-4 need
no hardware. Level 5 is the hardware session: full simplex pages through
`probe`, and the NAPS2 defects they exposed - see section 5.

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
read-image-count, image READ, ILI/EOM short-read trimming and flags, a
`03/80/13` READ whose filler data phase must be discarded, status
resynchronisation after a short data-in, and the `03/80/03` end-of-batch
sense. 28 assertions.

## 3. Session, synthetic device

```
build\mockscan.exe [outdir]
```

A self-answering fake iX1300 walks the whole batch state machine for colour
300 dpi, gray 200 dpi, line art 300 dpi, colour 150 dpi duplex and colour
**600 dpi** (5100x8400, 128 MB per page, which covers the unrestricted-size
path). The fake answers READ the way the real unit does (`build\duplex.log`):
separate front and back streams, `03/80/13` with a full filler data phase on
every third READ and whenever a duplex side runs more than 1 MB ahead of the
other, a pixel height longer than the sheet, and EOM + ILI on the last block.
Every page is compared byte for byte with the wire pattern of its side after
inversion and the 180-degree turn, plus height, side order, page counts and
clean termination. 66 assertions. Putting back any one of the three hardware
bugs below (no rotation, filler counted as image, front read before back)
makes it fail.

## 4. TWAIN, three ways

```
powershell -File tools\run-dsmtest.ps1     # sets up the scratch root and runs dsmtest
build\twaintest.exe build\ScanSnap-iX1300.ds   # direct load, Mock=1
```

`twaintest` performs exactly the capability writes NAPS2 does in
`TwainScanRunner.ConfigureSource` (XFERMECH=memory, INDICATORS=false,
FEEDERENABLED, DUPLEXENABLED, AUTOMATICSENSEMEDIUM, PIXELTYPE, BITDEPTH,
UNITS, XRES/YRES, BRIGHTNESS, CONTRAST), then ENABLEDS, the
XFERREADY/IMAGEINFO/IMAGEMEMXFER/ENDXFER loop, DISABLEDS, CLOSEDS. Each
memory strip must hold whole rows (`BytesWritten == Rows * BytesPerRow`) at
the running `YOffset`, and the strips must cover `ImageLength`, which is how
NAPS2 places them. 203 assertions, 0 failures; the old DS fails 20 of them.
Native DIB and BMP file transfer are covered too:
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

The scanner is attached and now runs on **WinUSB** (the rebind took effect
after the reboot): `Class = USBDevice`, `Service = WINUSB`, problem code 0,
and `SetupDi` enumerates it by the interface GUID. The driver logs:

```
winusb: bulk pipe 0x81 maxpacket 1024
winusb: bulk pipe 0x02 maxpacket 1024
winusb: opened \\?\usb#vid_04c5&pid_162c#8&2efe0359&0&7#{6e7c3d2a-...}
```

| Fact | Observed value |
|---|---|
| Device | `04c5:162c`, bcdDevice `0100` |
| Pipes | exactly two, both bulk: `0x81` IN, `0x02` OUT, 1024-byte packets |
| Channel alignment | 1024 read, 1024 write (queried while on usbscan) |
| Interrupt / event pipe | none (`EventChannelSize` = 0) |
| Handles | WinUSB grants one per interface - a second `CreateFile` is `ACCESS_DENIED` |

The pipe numbers the driver hardcodes are what the unit reports.

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

### The wedge was the pending reboot

Before the reboot the unit looked dead on bulk IN: writes completed, reads
never did, control transfers still worked, and no software sequence
(`CANCEL_IO`, `RESET_PIPE`, `CLEAR_FEATURE(HALT)`, `SET_INTERFACE`, draining
243 KB of queued data) changed that. The cause was not the firmware: the
WinUSB rebind was staged but not active, so `\\.\Usbscan0` still answered from
a half-switched driver stack while `SetupDi` saw no interfaces for our GUID.
`pnputil` had said so all along - *"Device is pending system reboot to complete
a previous operation"*.

After the reboot the unit runs on WinUSB and every command answers:

```
proto: inquiry 'FUJITSU' 'ScanSnap iX1300' rev '0M00' adf 1 flatbed 0
proto: vpd basic 600x600 max 600x600 window 5184x71080 color 1 gray 1 lineart 1
proto: colour interlace 0 accepted
proto: pixel size front 2550x3300
proto: sense key 0x03 asc 0x80 ascq 0x03     (feed: no documents)
```

The ban on `IOCTL_SET_TIMEOUT` in `src/usb.c` is explained rather than guessed:
with any non-zero `TimeoutRead` the usbscan path aborts every transfer with
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

Four more were found by driving the scanner from NAPS2:

4. **IMAGEINFO reported the wrong `BitsPerSample`.** It sent `{24,8,8,...}`
   for 24-bit colour, i.e. bits *per pixel* in the first slot. NAPS2 matches
   on `(PixelType, BitsPerPixel, SamplesPerPixel, BitsPerSample[0])` and threw
   `Unsupported pixel type: 24 2 3 24,8,8,0,0,0,0,0` on every page.
   `BitsPerSample` is per sample: `{8,8,8}`. `twaintest` checked none of
   those fields; it now checks all four, and the new assertion was confirmed
   to fail against the old code.
5. **A scan was gated on the hopper sensor.** `proto_feed` returned
   `PROTO_NODOCS` from `byte3 & 0x80` before sending anything. That bit reads
   "empty" on this unit with paper loaded, so no scan ever started. SANE
   pre-checks it only for the iX500; the feed's own `03/80/03` is
   authoritative. See PROTOCOL.md section 3a.
6. **`probe scan` opened the interface twice.** WinUSB grants one handle, so
   the session's `CreateFile` failed with `ACCESS_DENIED` and
   `session_create` returned NULL. It releases the probe's handle first.
7. **Mock defects the above exposed.** A new synthetic session inherited the
   previous session's response queue, and the no-documents path pre-pushed
   sense data that `do_cmd` then fetched again, leaving the mock one packet out
   of step for every later command.

### What real scans through NAPS2 exposed

Once paper fed, `probe scan` produced clean pages (`build\simplex.log`: every
READ full, the last one `eom 1 ili 1`, then `03/80/03`), but NAPS2 pages were
cut into horizontal bands and duplex sides were mostly solid gray. Three
defects, all fixed:

8. **Memory-transfer strips split rows.** `memory_xfer` filled the whole
   262144-byte buffer, which is not a whole number of rows (7650 bytes at
   300 dpi colour), reported `Rows = floor(bytes / BytesPerRow)` and advanced
   by the full buffer. NAPS2 places `Rows` rows at `YOffset`, so every strip
   after the first started mid-row: bands every 34.25 rows at 300 dpi (68.5 at
   150), each shifted sideways, with R/G/B rotated when the offset was not a
   multiple of 3. The probe path never used it, so its BMPs were fine. Strips
   now carry whole rows.
9. **"No image data yet" was counted as image.** In duplex about half of all
   READs answer `03/80/13`, and the unit still sends a full data phase of
   filler first. `read_side` added those bytes to the page, so a "page" was
   complete long before the sheet had passed (`build\duplex.log`: 32 real
   front blocks, 92 filler blocks in 0.3 s). `proto_read_image` now returns 0
   bytes for `03/80/13` and busy, as SANE does (`DEVICE_BUSY`, block dropped).
10. **Duplex read the whole front before the back.** The unit buffers both
    sides and stalls the one read too far ahead. Sides are now read
    alternately one block at a time, and a side ends when end of medium has
    been seen and its last READ was short - SANE's `DUPLEX_INTERLACE_ALT`
    rule, which is SANE's default for this generation.

Every NAPS2 page also came out rotated 180 degrees (not mirrored), front and
back, with the sheet loaded as the manual says. `page_from_raw` now turns
pages around.

### Paper path, empty feeder and the Scan button

`probe status` with each input loaded in turn gave byte 3 = `80` (nothing),
`00` (ADF), `81` (front slot), `01` (both); a Scan press set byte 4 bit 0 for
one 100 ms sample, a 3 s hold the same (PROTOCOL.md 3a/3b).

With the sensors read at batch start (`probe scan`, no options):

| Loaded | Log | Result |
|---|---|---|
| front slot | `build\auto_front.log` | `paper sensors front slot`, return path, 1 page 2550x3320, EOM+ILI end, `03/80/03` |
| 2 sheets ADF | `build\auto_adf.log` | ADF path, 2 pages 2550x3308/3307, `03/80/03` |
| nothing | `build\empty.log` (before the no-feed change) | feed answered `03/80/03`, rc 3 |

Earlier the NAPS2 run at 18:35:56 got `03/80/01` (jam) for a feed with
nothing in the ADF; the driver now refuses an empty batch before feeding.

The WinUSB open used to leave stale data-in queued: after a rejected
`08 … 01` paper query every command read the previous answer. The open now
drains the IN pipe (it discarded 244 bytes once, nothing on a clean open).

Watcher hand-off (`build\button.log`, `build\handoff.log`): the watcher opened
the unit, `probe info` asked for it and got it in the same millisecond, ran,
and the watcher reopened 5 s later.

Through NAPS2 with the installed DS (SHA-256 `B4A71813…275651`), duplex
300 dpi colour profile:

- nothing loaded: the "No paper is loaded" box, no motor, no NAPS2 error;
  the log holds only `paper sensors empty` (22:31:39).
- one sheet in the front slot: `paper sensors front slot`, both sides read
  alternately with `03/80/13` discarded, end on `03/80/03`; `test3.pdf` has 2
  pages of 2550x3300, both with content (22:32:41).

Scan button (`ScanSnapButton.exe`, installed with `--install`): presses at
22:45:45 (NAPS2 open) and 22:45:58 (NAPS2 closed) each ran
`NAPS2.exe "/StiDevice:ScanSnap iX1300"`; the open window scanned and added
the pages, and with NAPS2 closed it started and scanned.

### Confirmed through NAPS2 on the unit

With the rebuilt DS installed, three NAPS2 batches at 300 dpi colour, Letter
(duplex 1 sheet, simplex 1 sheet, duplex 2 sheets) gave 2, 1 and 4 pages,
each batch ending on `03/80/03`. The protocol log shows front and back READs
alternating, 98 full READs per side (3300 lines), and all 301 `03/80/13`
answers recorded as `got 0`. In the delivered pages the image breaks no longer
fall on the 262144-byte strip grid, no row is filler gray, and every page with
content matches the raw probe scan turned 180 degrees.

### Still open on hardware

A short sheet in **duplex** - EOM + ILI ending one side of the alternating
read loop early - has only run on the synthetic device; the simplex case has
run on the unit (table above).

Line art, gray and 600 dpi have run through NAPS2 only on the old DS, with
the strip defect.

This unit's VPD reports `duplexOffset = 0` even though the hardware is duplex
(two CIS, 60 ipm), so `0x2e/0x2f` is not the field the iX1600 profile puts
there. Nothing gates on it - `CAP_DUPLEX` is answered unconditionally - but
the real bit is unidentified.

## 6. Install state

Installed and verified:

| Step | Result |
|---|---|
| `C:\Windows\twain_32\ScanSnap-iX1300.ds` | current build (SHA-256 `C3B978C5...`), with the strip, read-loop and rotation fixes |
| `HKLM\SOFTWARE\WOW6432Node\TWAIN\DataSources` | `ScanSnap-iX1300` = the .ds path |
| Driver package | `oem31.inf` published, device assigned to it |
| Catalog | SHA-256, signed, `signtool verify /pa` clean |
| Certificate | in `LocalMachine\{My,Root,TrustedPublisher}` with a persisted key |
| Device | `Class = USBDevice`, `Service = WINUSB`, problem code 0 |

Elevation works non-interactively here: `Start-Process -Verb RunAs` with the
full path to `System32\WindowsPowerShell\v1.0\powershell.exe` returns, so
`install\install.ps1` runs end to end.

### The WinUSB switch needs a reboot

`pnputil` says so directly: *"Device is pending system reboot to complete a
previous operation"* (exit 50 for `/restart-device`). Until then the picture is
inconsistent, and that inconsistency is worth knowing about:

| Signal | Reads as |
|---|---|
| Device class / service | `USBDevice` / `WINUSB` - the new binding |
| `DeviceInterfaceGUIDs` on the device key | our GUID, present |
| `SetupDiGetClassDevs` for that GUID | **zero interfaces** |
| `\\.\Usbscan0` | still openable, though no usbscan device node exists |

So the driver still falls back to the stale usbscan symlink and sees the wedged
unit. After the reboot the WinUSB interface should enumerate, and the winusb
path - whose pipe reset was just fixed - becomes the live one.

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

Simplex and duplex colour scans through NAPS2 are proven on hardware with the
current DS (section 5); a sheet shorter than the window, and the other modes
on the current DS, are the remaining gaps.

`dsmtest` proves discovery, `OPENDS`, capabilities, transfer and close through
NAPS2's real `twaindsm.dll`, so the TWAIN path itself does not depend on any of
the above.
