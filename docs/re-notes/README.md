# Reverse-engineering record

## Target set

| Artefact | SHA-256 / identity | Role |
|---|---|---|
| `C:\Windows\System32\SSiX13-x64.dll` | `f3bad87d8d5644ff5d4377556d8bfc021c4c364fce775f9b19bcb39c85c63a3d` | PFU WIA/STI USD (x64, 35 KB) |
| `…\PFU\ScanSnap\Home\SSDriver\fi5110\fjtw32.dll` + `Mercury.dll` | x86, exports `DS_Entry` / 61 `SC_*` | **legacy** TWAIN DS + command layer; ScanSnap Home does not use it for the iX1300 |
| `…\SSDriver\icube\DeviceCtl\{ScanSnap,DeviceCtl,DeviceCt2,DeviceCt3}.dll` | x86 | iCube device control + capability model |
| `…\SSDriver\icube\I3ipCore\*.i3u` | x86 | image processing steps |
| `C:\Windows\INF\oem21.inf` (`WIAPFU.INF`) | DriverVer 02/12/2026 2.0.0.3 | binding: `USB\VID_04C5&PID_162C` -> `usbscan.sys` + USD `{4EA5AFE9-…}` |
| Device instance | `USB\VID_04C5&PID_162C\8&2EFE0359&0&7` | `Present = False` during analysis |

## REA evidence

`rea analyze` on the USD (`--provider ghidra`, Ghidra 12.1.4):

```
evidence_id   ev_cee1d042372c029a6fb26408b149a18b436c9e87b9292bc24ea350efdbbae091
subject       SSiX13-x64.dll, pe, x86_64, sha256 f3bad87d…
operation     binary_overview
result        8 segments, .text 0x1000-0x8000 (28 672 B), 142 procedures, 343 strings
```

`rea function` on `DllGetClassObject`:

```
evidence_id   ev_734f6a981f2d34d5aeae0f430a79d74ede00874c13eda5814e4bb01903818b69
provider      ghidra 12.1.4
```

The overview plus the extracted strings are enough because the USD is a thin
`usbscan.sys` client: its own strings name every operation it performs
(`FJSTI_RAWREAD`, `FJSTI_RAWWRITE`, `FJSTI_SENDCOMMAND`, `FJSTI_GETVERSION`,
`FJSTI_NEEDDISTORTION`, `FJSTI_EVENTSETTING`, `SSMD CreateFileA (%s)`), and its
baked-in source path is `…\minidriver\ix1300\stisrc\fjscmusd.cpp`. Decompiling
each wrapper would confirm what the strings already state: forward a SCSI CDB,
read or write a data block, read a 13-byte status packet.

## Secondary sources (command layouts and constants)

- `sane-backends` backend `fujitsu` (`fujitsu.c`, `fujitsu-scsi.h`): the
  0x43/0x53 envelope, CDB offsets, mode pages, `SET WINDOW` descriptor layout,
  sense classification, and the fact that the iX1300 needs **no** `init_model()`
  overrides (the iX1300/iX1400 commits touched only `fujitsu.conf.in` and
  `fujitsu.desc`).
- `holgerkrupp/Scan` (`FujitsuScanSnapDevice.swift`,
  `FujitsuScanSnapModelProfile.swift`): the iX1600-generation profile the
  iX1300 inherits, and the `ix1600-front-gray-150.transcript` used as the
  scripted test vector here.
- `mmacpherson/s1500d` `docs/protocol.md`: envelope + `GET HW STATUS` bit map
  and endpoint shape of the same transport.

## Evidence gaps

- USB descriptors of the iX1300 itself (endpoint addresses, packet sizes,
  interface count) were not readable because no unit was attached; the driver
  enumerates bulk pipes at runtime instead of hard-coding them.
- The vendor DS `fjtw32.dll` could not be executed end-to-end (it needs
  `PfuSsMon.exe` to have opened the unit), so its capability table was read
  from strings and the INF rather than observed.

## ScanSnap Home's control stack (2026-10-25)

REA 6.4.0 + Ghidra 12.1.4, static only (no vendor binary run, no USB trace).
Full per-binary reports were session artefacts; the conclusions are here.

```
PfuSsMon.exe -> SshCtlWrp.dll -> SshCtl.dll (CSshCtlObjForPSIP, model 0x7e = iX1300)
  -> fjictwin.dll!DS_Entry (iCube TWAIN DS, identity set by DAT 0x8004 MSG_SET)
  -> DeviceCtl.dll -> DeviceCt2.dll -> DeviceCtl\ScanSnap.dll (SC_* CDB builders)
  -> STI Escape FJSTI_SENDCOMMAND -> SSiX13-x64.dll -> usbscan.sys
```

| Question | Finding | Evidence |
|---|---|---|
| Return path select | `F1 0F 02 00 00 00 00 00 00 00` (= SANE `SC_function_rpath` 0x2f), then `F1 0A 00…` up to 20 times, then the ordinary `31 01` feed | ScanSnap.dll builder `100314c0` ev_718f796c…; DeviceCt2 `1008f918`, `1008d2a7`. **On the iX1300**: `F1 0F 02` + feed works; `F1 0A` answered `03/80/01` and is not sent |
| When return path is chosen | C2 (22 bytes) `(raw[3] & 0x81) == 0x81`: U-turn hopper empty **and** front slot loaded; both loaded = ADF | DeviceCt2 `1008e1f1`, model-0x1d (fi-800R) branch. The iX1300's sensors match this layout (PROTOCOL.md 3a) |
| Empty feeder | Host-side: `(raw[3] & 0x81) == 0x80` -> synthesise sense 03/80/03 (`SC_SetHopperEmptyError` writes saved sense, sends nothing); 03/80/01 itself is always "jam" | ScanSnap.dll `1004fcf0` ev_62d42217…, sense map `10049ce0` ev_53b1d2c7… |
| Monitor pre-check | for the iX1300 PfuSsMon turns an "empty" status into "ready" and lets the DS decide | PfuSsMon `CheckSsDev @455930` |
| Scan button | poll `C2 00 00 00 00 00 00 00 30 00` (48 bytes) every 200 ms with the DS kept open; `raw[4] & 0x01` press, `& 0x20` long press; `raw[14] & 0x40` scan end, `& 0x08` cancel | SshCtl PSIP `GetADFStatus @1001ec10` ev_c03613c3…; PfuSsMon timer 10 `@476c60/@483890` |
| Poll suppression | existence of `Local\PFUSSHMonButtonPollingSkip` skips a poll; `Local\PFUSSHMonScanButtonSkip` ignores a detected press (both created by Home's .NET UI) | PfuSsMon `OnTimer @483890`, `OnSwitchPush @47b870` |
| USD button events | none: `SetNotificationHandle` starts a thread that only watches a directory; `GetNotificationData` never gets a device event | SSiX13 `FUN_1c304fa8` ev_ecc9f705…, `FUN_1c305730` ev_794e248d…, `FUN_1c305168` ev_f117b10d… |

Paper-presence query `08 00 00 00 01 00` (ScanSnap.dll `SC_Escape 0x800`,
capability-gated) is **not** usable on the iX1300: tried once, the unit
answered with more than the 1-byte data phase and every later command read the
previous answer until the WinUSB open-time drain was added.
