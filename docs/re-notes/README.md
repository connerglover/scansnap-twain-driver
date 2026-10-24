# Reverse-engineering record

## Target set

| Artefact | SHA-256 / identity | Role |
|---|---|---|
| `C:\Windows\System32\SSiX13-x64.dll` | `f3bad87d8d5644ff5d4377556d8bfc021c4c364fce775f9b19bcb39c85c63a3d` | PFU WIA/STI USD (x64, 35 KB) |
| `…\PFU\ScanSnap\Home\SSDriver\fi5110\fjtw32.dll` | x86, 4 116 848 B, exports `DS_Entry`, `FJTW32.DS` | vendor TWAIN data source |
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
