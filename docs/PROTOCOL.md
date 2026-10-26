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
| `03/80/13` | no image data yet | discard the block (the unit still sends a full data phase of filler), read again |
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
| `0xc2` | GET HW STATUS | 12 B classic, 48 B as ScanSnap Home polls; see 3b |
| `0xf1` | SCANNER CONTROL | function = byte1 low nibble + byte2 high nibble: `00` ADF (U-turn), `04` cancel, `05` lamp on, `0a` (vendor sends before a return-path feed; meaning unknown), `2f` return path (`F1 0F 02`); byte1 bit 4 + `0x10` = read-image-count |
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

## 3a. Paper sensors, paper path and an empty feeder

**Observed** with `probe status` (48-byte C2, polled every 100 ms) while
loading and unloading each input:

| Loaded | byte 3 | Driver |
|---|---|---|
| nothing | `80` | no documents, **no feed issued** |
| U-turn ADF only | `00` | ADF path (`F1 00`) |
| front Return Scan slot only | `81` | return path (`F1 0F 02`) |
| both | `01` | ADF path, as ScanSnap Home does |

The sensors are read once, at the start of a batch (`proto_begin`). Between
sheets the feed's own answer (`03/80/03`) ends the batch: the unit may have
picked the next sheet past the sensor already.

**Why not just feed:** on an empty unit the feed answers `03/80/03` at some
times and `03/80/01` - the paper-jam code - at others (NAPS2 run at 18:35:56,
`%TEMP%\ScanSnapTWAIN.log`), so the feed cannot tell "no paper" from a jam,
and it runs the motor. ScanSnap Home makes the same check before feeding
(re-notes). A sheet in the front slot with the ADF path selected also answers
`03/80/01`.

An earlier note here called byte 3 bit 7 "advisory" because it read `80` with
"paper in the ADF". With both sensors known, that reading means nothing was in
the U-turn hopper - most likely the sheet sat in the front slot.

**Return path, observed**: `F1 0F 02` before mode select/window, then the
ordinary `31 01` feed and `SCAN`: the sheet in the front slot goes through and
out, one page 2550x3319 at 300 dpi ending with EOM + ILI, then `03/80/03`.
After a return-path batch the unit pulls a newly inserted front-slot sheet in
part way and byte 2 reads `80` (meaning not established).
ScanSnap Home's extra `F1 0A` before the feed answered `03/80/01` here and is
not sent.

## 3b. GET HW STATUS bits

From sane-backends (`get_GHS_*`) and ScanSnap Home (re-notes); **observed** on
the iX1300 where marked:

| Byte | Bit | Meaning | Observed |
|---|---|---|---|
| 3 | 7 | U-turn hopper empty | yes |
| 3 | 0 | front Return Scan slot loaded | yes |
| 3 | 5 | ADF/cover open | - (closing the chute powers the unit off) |
| 4 | 0 | Scan button pressed; latched, cleared by the next C2 read | yes: one 100 ms sample per press |
| 4 | 5 | Scan button long press | **no**: a 3 s hold also sets only bit 0 |
| 4 | 7 | sleep | yes |
| 14 | 6 / 3 | scan end / scan cancel (48-byte form) | - |

Idle, nothing loaded, asleep: `00 00 00 80 80 00 …`.

The button raises no event on the USB side (two bulk pipes, no interrupt
pipe) and PFU's USD reports none either; `src/button.c` polls byte 4 every
200 ms, as ScanSnap Home's monitor does.

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
- duplex sides are read **alternately**, one block each: the unit buffers both
  and answers `03/80/13` on the side read too far ahead. A side ends once a
  READ has reported EOM and that side's last READ was short (ILI). This is
  SANE's `DUPLEX_INTERLACE_ALT`, its default for every ScanSnap.
- the pixel-size READ returns the **window** length (4200 lines for a 14"
  window at 300 dpi); the real sheet ends earlier with EOM + ILI, the
  information field holding the residue of the requested length
- both sides arrive rotated 180 degrees for a sheet loaded as the manual says
  (observed through NAPS2, front and back, not mirrored)

## 5. Established on hardware

Read from the attached unit (`04c5:162c`, `usbscan.sys`):

- **USB descriptors**: exactly two pipes, both bulk, `0x81` IN and `0x02` OUT,
  1024-byte max packet, no interrupt/event pipe, 1024-byte channel alignment.
  The driver's defaults match; WinUSB enumerates the same pair.
- **INQUIRY**: vendor `FUJITSU`, product `ScanSnap iX1300`, ROM revision
  `0M00` (the mock had `1.50`), ADF present, no flatbed.
- **VPD**: basic resolution 600x600, max 600x600, window limit 5184x71080
  units, colour + gray + line art. The standard-resolution bitmap is the unit's
  own; the hardcoded 150-1200 dpi fallback list is wrong for it and is only
  used if VPD fails entirely.
- **Colour interleaving**: `SET WINDOW` with order `0` (RGB dot) is accepted.
- **Pixel size** after `SCAN` at 300 dpi: 2550x4200 for a 14" window; a
  Letter sheet then ends after 3315 lines with EOM + ILI.
- **A full simplex page** (`build\simplex.log`): 97 READs of 260100 bytes
  (34 lines each) with no `03/80/13`, the last one `eom 1 ili 1`, then the
  next feed answers `03/80/03`. Colour, gray, line art and 600 dpi also
  completed through NAPS2.
- **Duplex READs** (`build\duplex.log`): `03/80/13` on roughly half of all
  READs on both sides, each still carrying a full data phase.
- **`0x2e/0x2f` is not the duplex field.** The unit is a duplex scanner (two
  CIS, 60 ipm) but the value there reads 0, so the iX1600 model is wrong at
  that offset. Nothing depends on it: `CAP_DUPLEX` is answered
  `TWDX_1PASSDUPLEX` unconditionally and `j.duplex` comes from
  `CAP_DUPLEXENABLED`. Where the real capability bit lives is still open.

`0M00` also contradicts the mock, which was written from the iX1600 profile.

## 6. Not established on hardware

- A side ending early (EOM + ILI) in the alternating **duplex** read loop:
  NAPS2's Letter window ended every side on a full buffer. The simplex case
  has run on the unit (`probe scan`, 3308-3320 lines of a 4200-line window).
