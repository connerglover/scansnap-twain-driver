# Verification

Four levels, each with the command that proves it. Everything below was run in
this session; the scanner itself was **not** attached, which is the one gap.

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
read-image-count, image READ, ILI short-read trimming, and the `03/80/03`
end-of-batch sense. 21 assertions.

## 3. Session, synthetic device

```
build\mockscan.exe [outdir]
```

A self-answering fake iX1300 walks the whole batch state machine for colour
300 dpi, gray 200 dpi, line art 300 dpi and colour 150 dpi duplex: geometry,
row width per bit depth, TWAIN pixel type, page counts (2 / 4) and clean
termination. With `outdir` it writes BMPs; those were inspected for polarity
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
assertions, 0 failures.

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

Because `%WINDIR%\twain_32` needs admin rights, `dsmtest` patches the DSM's
`GetWindowsDirectoryA` import to point at a scratch root that holds
`twain_32\ScanSnap-iX1300.ds` plus a copy of `twaindsm.dll`;
`tools\run-dsmtest.ps1` creates that tree. The production install writes the
real directory instead.

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
`%WINDIR%\twain_32`. `dsmtest` exercises those same triplets against the same
`twaindsm.dll` with the DS present, and they succeed. Installing the DS into the
real directory is the only remaining difference, and it is the step that needs
administrator rights.

## 5. Hardware (still to run)

```
build\probe.exe info              # device path, INQUIRY, VPD, HW status
build\probe.exe status            # live status; press the scan button
build\probe.exe scan out         # one 300 dpi colour page
```

Then NAPS2: *New Profile > Driver: TWAIN > Device: ScanSnap iX1300*.

## Remaining unknowns

- **Real USB descriptors.** The scanner was unplugged
  (`USB\VID_04C5&PID_162C\8&2EFE0359&0&7`, `Present = False`), so pipe
  numbers/packet sizes are enumerated at runtime rather than asserted. The
  transport prints them to the log on the first real open.
- **Endpoint count.** Both `usbscan.sys` and WinUSB are handled; if the unit
  turns out to expose an interrupt endpoint it is simply unused.
- **Colour interleaving on the unit.** Probed at runtime (RGB dot, BGR dot,
  line) exactly as SANE does; the model profile predicts RGB dot.
- **Straight-path (return scan) mode.** Only the U-turn ADF is driven, matching
  the iX1600 profile this generation shares. `install\ScanSnap-iX1300.ini`
  can pin the interlace if a unit disagrees.
