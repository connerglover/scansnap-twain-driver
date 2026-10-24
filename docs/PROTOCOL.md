# ScanSnap iX1300 - protocol notes

Everything here was established from the shipped binaries plus two independent
open-source implementations of the same generation. Statements are marked
**observed** (read out of a binary/dump/transcript) or **inferred**.

## 1. Where the USB conversation actually happens

| Layer | Component | Note |
|---|---|---|
| Kernel | `usbscan.sys` | in-box STI class driver; exposes the bulk pair via `ReadFile`/`WriteFile` |
| WIA/STI minidriver | `C:\Windows\System32\SSiX13-x64.dll` (35 KB, x64) | PFU USD; **only** wrapping `DeviceIoControl`/`ReadFile`/`WriteFile` around SCSI commands |
| TWAIN DS (32-bit) | `…\SSDriver\fi5110\fjtw32.dll` (4.1 MB, x86) | the vendor data source ScanSnap Home registers |
| iCube engine (32-bit) | `…\SSDriver\icube\fjictwin.dll`, `DeviceCtl\ScanSnap.dll`, `I3ipCore\*.i3u` | PaperStream IP core + image processing |

`SSiX13-x64.dll` strings (observed): `Called UsdFJSCMDevice::Escape() : FJSTI_RAWREAD`,
`… FJSTI_RAWWRITE`, `… FJSTI_SENDCOMMAND (0x%02x)`, `FJSTI_GETVERSION`,
`FJSTI_NEEDDISTORTION`, `FJSTI_EVENTSETTING`, `SSMD CreateFileA (%s)`.
Source path baked into the PDB name: `…\driver_branch\maple\driver1\minidriver\ix1300\stisrc\fjscmusd.cpp`.
Conclusion (inferred, strongly): the USD contains **no** device protocol of its
own; it forwards SCSI CDBs through usbscan. Reimplementing it means
reimplementing what `fjtw32.dll` sends, not what the USD does.

The INF (`C:\Windows\INF\oem21.inf` = `WIAPFU.INF`) confirms the binding:

```
[SSIX1300.Scanner]
Include=sti.inf
Needs=STI.USBSection
DeviceSubType=0x0103
[SSIX1300.DeviceData]
TwainDS="FUJITSU ScanSnap iX1300"
[SSIX1300.Scanner.AddReg]
HKR,,NTMPDriver,,"usbscan.sys"
HKR,,USDClass,,"{4EA5AFE9-B383-4B07-BCD3-A271228B5FE5}"
```

Note the INF *exports* a TWAIN DS name, but ScanSnap Home 4.1 registers nothing
under `HKLM\SOFTWARE\WOW6432Node\Twain` (observed); `fjtw32.dll` is loaded by
`PfuSsMon.exe`, so a TWAIN application could not find it without the monitor.

## 2. Wire format

Observed in `sane-backends/backend/fujitsu-scsi.h` and reproduced verbatim by
the iX1600 transcript this generation matches:

| Direction | Size | Contents |
|---|---|---|
| bulk OUT | 31 | `0x43`, zeros, CDB at offset **0x13** |
| bulk OUT | N | command data-out (mode page / window block / scan window list) |
| bulk IN | N | command data-in |
| bulk IN | 13 | `0x53`, status byte at offset **0x09** |

Status 0 = good, 8 = busy, otherwise CHECK CONDITION -> `REQUEST SENSE (0x03)`
with 0x12 return bytes. Sense key/ASC/ASCQ decide "no documents"
(`03/80/03|20|30`), "no data yet" (`03/80/13`), "cover open" (`03/80/02`),
paper jam (`03/80/01|04|07|08`), and the benign end-of-page
(`key 0, ASC 0, EOM or ILI`).

## 3. Command set this generation uses

| Opcode | Name | Payload |
|---|---|---|
| `0x00` | TEST UNIT READY | —; issued after every feed |
| `0x03` | REQUEST SENSE | 0x12 bytes back |
| `0x12` | INQUIRY | 96 bytes; vendor at +8, product at +16, ROM rev at +32 |
| `0x12/EVPD 0xF0` | VPD | resolutions, window limits, modes, duplex offset |
| `0x15` | MODE SELECT | pages `0x3c` (auto/ALD), `0x38` (double feed), `0x39` (dropout), `0x3a` (buffer) |
| `0x24` | SET WINDOW | 8-byte header + 64-byte descriptor (×2 for duplex) |
| `0x1b` | SCAN | list of window ids (`0x00`, `0x80`) |
| `0x31` | OBJECT POSITION | 1 = feed, 4 = halt |
| `0x28` | READ | datatype `0x80` = pixel size (32 B), `0x00` = image data |
| `0xc2` | GET HW STATUS | 12 bytes; byte 3 bit 7 = hopper empty (inverted) |
| `0xf1` | SCANNER CONTROL | 0 = ADF, 4 = cancel, 5 = lamp on; `0x10` = read-image-count |
| `0x2a` | SEND | LUT `0x83`, JPEG q-table `0x88` (only the iX500 needs these) |

Window descriptor (offsets within the 64-byte block, observed in both
implementations):

```
0x00 window id     0x02 Xres     0x04 Yres
0x06 ULX            0x0a ULY      0x0e width      0x12 length   (1/1200 inch)
0x16 brightness     0x17 threshold 0x18 contrast
0x19 composition (0 lineart, 2 gray, 5 colour)   0x1a bits per pixel
0x20 compression type (0 none, 0x81 hardware JPEG)   0x21 compression arg
0x28 vendor block (0x00 mono, 0xc1 colour)
0x29 gamma (0 internal curve, 0x80 downloaded LUT)
0x2a scanning order (0 line, 1 dot)  0x2b order argument (0 RGB, 5 BGR)
0x35 paper select (0xc0 non-standard) 0x36 paper width 0x3a paper length
```

## 4. Behaviour that matters (iX1300 = iX1600 generation)

- **No pre-read diagnostic**, **no JPEG quantisation table**, **no gamma
  download** when brightness/contrast are neutral: the scanner uses its
  internal curve, identified by `0x29 = 0`.
- TEST UNIT READY after `OBJECT POSITION feed`, before `SCAN`.
- Colour interleaving is **probed**: try RGB dot, BGR dot, line order, keep
  whichever `SET WINDOW` accepts.
- Colour and grayscale samples arrive **inverted** (0 = paper white).
  Line art arrives **1 = ink**, MSB first.
- A batch that fed at least one sheet is closed with `SCANNER CONTROL cancel`
  once the feeder runs empty; the feeder's `03/80/03` sense is the normal end.
- Hardware JPEG is available (`0x20 = 0x81`) but is not used by this driver:
  NAPS2's memory path wants uncompressed samples.

## 5. What was verified against hardware

Nothing, in this session: the scanner was not attached (PnP instance
`USB\VID_04C5&PID_162C\8&2EFE0359&0&7`, `Present = False`). The command
sequence is validated by `tools/selftest.exe` against a scripted transport and
`tools/mockscan.exe` against a synthetic device; see `docs/VERIFICATION.md`.
