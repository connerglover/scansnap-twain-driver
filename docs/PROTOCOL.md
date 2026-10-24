# ScanSnap iX1300 - reverse-engineered protocol

**observed** = read out of a shipped binary, a byte transcript, or a live
device response. **inferred** = follows from those but was not itself seen.

## 1. Stack

```
NAPS2 (x64)
  └ NAPS2.Worker.exe (x86)          TWAIN runs 32-bit
      └ twaindsm.dll 2.3            data source manager
          └ ScanSnap-iX1300.ds      this project
              └ WinUSB / usbscan.sys
                  └ iX1300  04c5:162c
```

What ScanSnap Home installs (**observed**):

| File | Bitness | Role |
|---|---|---|
| `C:\Windows\System32\SSiX13-x64.dll` | x64, 35 KB | WIA/STI minidriver ("USD") |
| `…\SSDriver\fi5110\fjtw32.dll` | x86, 4.1 MB | vendor TWAIN DS, exports `DS_Entry` |
| `…\SSDriver\icube\fjictwin.dll`, `DeviceCtl\*.dll`, `I3ipCore\*.i3u` | x86 | iCube control + image processing |
| `C:\Windows\INF\oem21.inf` | — | binds `USB\VID_04C5&PID_162C` to `usbscan.sys` + the USD |

The USD is a pass-through. Its own strings (**observed**):
`Called UsdFJSCMDevice::Escape() : FJSTI_RAWREAD`, `FJSTI_RAWWRITE`,
`FJSTI_SENDCOMMAND (0x%02x)`, `SSMD CreateFileA (%s)`, plus its baked-in source
path `…\minidriver\ix1300\stisrc\fjscmusd.cpp`. So all device semantics live one
layer up, in `fjtw32.dll` - which is what this driver reimplements.

Also **observed**: ScanSnap Home registers *no* TWAIN data source
(`HKLM\SOFTWARE\WOW6432Node\Twain` absent) and only `wiatwain.ds` sits in
`C:\Windows\twain_32`. `PfuSsMon.exe` holds the device open, which is why the
old engine-based driver needed Home running.

## 2. Wire format

**observed** in sane-backends (`fujitsu-scsi.h`) and reproduced byte-for-byte by
the iX1600 transcript that this generation matches:

| Direction | Size | Contents |
|---|---|---|
| bulk OUT | 31 | `43`, zeros, CDB at offset **0x13** |
| bulk OUT | N | data-out (mode page, window block, window id list) |
| bulk IN | N | data-in (INQUIRY, VPD, pixel size, image) |
| bulk IN | 13 | `53`, SCSI status at offset **0x09** |

Status 0 = good, 8 = busy, else CHECK CONDITION -> `REQUEST SENSE (0x03)`,
0x12 bytes. Sense classification (**observed**):

| key/ASC/ASCQ | Meaning | Driver result |
|---|---|---|
| `02/00/00` | not ready | busy, retry |
| `00/00/xx` with EOM or ILI | end of page | success, honour ILI length |
| `03/80/13` | no image data yet | poll again |
| `03/80/03`, `20`, `30` | no documents | feeder empty |
| `03/80/02` | cover / ADF open | interlock |
| `03/80/01`,`04`,`07`,`08` | jam | paper jam |

## 3. Commands

| Opcode | Name | Notes |
|---|---|---|
| `0x00` | TEST UNIT READY | before INQUIRY, and again after each feed |
| `0x03` | REQUEST SENSE | 0x12 bytes |
| `0x12` | INQUIRY | 96 B; vendor +8, product +16, ROM rev +32, `+0x2d` ADF/flatbed bits |
| `0x12` EVPD `0xf0` | VPD | +0x05/07 basic res, +0x0a/0c max res, +0x14/18 window limits, +0x1c mode bits, +0x12/13 standard resolutions |
| `0x15` | MODE SELECT | pages `0x3c` (auto/ALD), `0x38` (double feed), `0x39` (dropout), `0x3a` (buffer) |
| `0x24` | SET WINDOW | 8-byte header + 64-byte descriptor per side |
| `0x1b` | SCAN | data-out is the window id list (`00`, `80`) |
| `0x31` | OBJECT POSITION | `01` feed, `04` halt |
| `0x28` | READ | datatype `0x80` pixel size (32 B), `0x00` image (3-byte length) |
| `0xc2` | GET HW STATUS | 12 B; `byte3 & 0x80` = hopper **empty** |
| `0xf1` | SCANNER CONTROL | `00` ADF, `04` cancel, `05` lamp on; bit 4 set + `0x10` = read-image-count |
| `0x2a` | SEND | LUT `0x83`, JPEG q-table `0x88` (only iX500/iX100 need these) |

Window descriptor (**observed** in both implementations):

```
00 id        02 Xres      04 Yres      06 ULX   0a ULY
0e width     12 length                              (1/1200 inch)
16 brightness (128 = neutral)   17 threshold   18 contrast (128 = neutral)
19 composition  (0 line art, 2 gray, 5 colour)     1a bits per pixel
20 compression type (0 none, 0x81 hardware JPEG)    21 compression arg
28 vendor block (00 mono, c1 colour)
29 gamma (0 = internal curve, 0x80 = downloaded LUT)
2a order (0 line, 1 dot)  2b order arg (0 RGB, 5 BGR)
35 paper select (0xc0 non-standard)  36 paper width  3a paper length
```

## 4. Generation behaviour (iX1300 == iX1600 profile)

**observed**: SANE's iX1300 and iX1400 support arrived as *docs + config only*
commits, with no `init_model()` override; the iX500/iX100 are the only ScanSnaps
with quirks. holgerkrupp/Scan states the iX1300 "inherits the iX1600 profile"
and pins its command sequence to the iX1600 transcript.

Consequences:

- **no** `SEND DIAGNOSTIC "SET PRE READMODE"`, **no** JPEG quantisation table,
  **no** gamma download while brightness/contrast are neutral (`0x29 = 0`)
- TEST UNIT READY after `OBJECT POSITION feed`, before `SCAN`
- colour interleaving is **probed** (RGB dot, BGR dot, line), keeping whichever
  `SET WINDOW` accepts
- colour and gray samples arrive **inverted** (0 = paper white); line art is
  1 = ink, MSB first, whole-byte rows
- a batch that fed a sheet is closed with `SCANNER CONTROL cancel` when the
  feeder runs empty; the `03/80/03` sense *is* the normal end of batch

## 5. Not established on hardware

- Actual USB descriptors (endpoint numbers, packet sizes, interface count,
  interrupt endpoint). The driver enumerates bulk pipes at runtime and logs them,
  so it does not depend on the guess.
- What the iX1300's INQUIRY/VPD actually returns; parsing falls back to A4 /
  150-1200 dpi if VPD fails.
- Which colour interleaving the unit accepts (probed).
- Straight-path / return-scan mode: only the U-turn ADF path is driven.
