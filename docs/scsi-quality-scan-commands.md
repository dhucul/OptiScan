# Optical-drive quality-scan SCSI command reference

Vendor SCSI commands OptiScan uses (or can use) to measure disc quality, recovered by
reverse-engineering Vinpower's `vpscan.exe` / `libqscan_*.dll` (a port of the QPxTool scan
engine) and **hardware-validated on a PLEXTOR PX-891SAF PLUS** (a MediaTek/PLDS drive).

All commands are 12-byte vendor CDBs unless noted, issued through
`ScsiDrive::SendSCSIWithSense` (`ScsiDrive.Core.cpp`).

## Which engine each drive uses

| Chipset / brand | Quality-scan opcode | OptiScan backend |
|---|---|---|
| Classic Plextor (PX-708–760) | `0xE9` / `0xEB` Q-Check | `ScsiDrive.QCheck.cpp` |
| **Lite-On / LG / PLDS / MediaTek** (incl. **PX-880/890/891**) | **`0xDF`** (old) · `0xF3 0E` (new) | `ScsiDrive.LiteOnScan.cpp`, `ScsiDrive.LiteOnJitter.cpp`, `ScsiDrive.LiteOnFeTe.cpp` |
| Pioneer | `0x3B` / `0x3C` buffer (id `0xE1`) | `ScsiDrive.PioneerScan.cpp` |
| Any MMC (fallback) | `0xBE` READ CD + software C2 | `OpticalDrive_C2Scan.cpp` |

The PX-891SAF PLUS is **PLDS**, so it scans via the **`0xDF`** family — not the `0xE9`
Plextor path.

## `0xDF` — PLDS / Lite-On / LG (the PX-891's channel)

Sub-command is CDB `byte[1..2]`. Direction is data-in unless noted. Status column is the
result observed on the PX-891SAF PLUS.

| Sub-cmd | Function | Len | PX-891 status |
|---|---|---|---|
| `DF 00 0F` | drive state registers | 128 B | ✓ GOOD + data |
| `DF A0` / `DF A3 01` | errc measurement **start / stop** (data-out, len 0) | 0 | ✓ arm / disarm |
| `DF 82 09` | latch CD error counters | 256 B | ✓ GOOD + data |
| `DF 82 05` | read CD counters → **C1 / C2 / CU** | 256 B | ✓ GOOD + data |
| `DF 97` | interval counter reset | 256 B | ✓ used in scan |
| `DF 1B 80` | jitter / beta readback (LBA in CDB[4..7]) | 16 B | opcode accepted (needs LBA) |
| `DF 08 01` / `DF 08 02` | focus/tracking error init / read (LBA in CDB[4..7]) | 16 B | opcode accepted (needs LBA) |
| `DF 02 09` | FE/TE position | 64 KB* | ✓ GOOD + data |

`*` `DF 02 09` requests `0x10000`; the drive clamps to the 16-bit ATAPI transfer ceiling.

### Validated C1/C2 scan sequence (`ScsiDrive::LiteOnScanPoll`)

```
arm      DF A3 00                    clear
         DF A0 00 00 02  /  00 00 04 start C1/C2 measurement
per iv:  READ the interval           <-- REQUIRED: the MediaTek counters only tally
                                          sectors the host reads (BE READ CD, audio),
                                          chunked <= 16 sectors (under the 0xFFFE cap)
         DF 82 09                    latch
         DF 82 05                    read C1/C2/CU
         DF 97                       reset interval, advance LBA += 75
disarm   DF A3 01
```

**The read step is the fix this repo added** — without it the counters stay idle at zero
(the "0xDF accepted but trial reads returned all zeros" failure). See
`ScsiDrive::LiteOnScanDriveHead`.

### Counter buffer field map (`DF 82 05` reply, big-endian)

| Field | Offset | Type |
|---|---|---|
| C1 (BLER) | `0x00` | u16 |
| C2 | `0x02` | u16 |
| CU (uncorrectable) | `0x04` | u8 |

> Note: C2 > C1 was observed in defect zones on the validation disc, which is atypical for a
> strict C1⊇C2 relationship — the exact C1↔C2 assignment is worth confirming against
> QPxTool's `cd_errc` struct. The values are unambiguously live and disc-dependent.

## `0xEA` — classic Plextor Q-Check (reference; not on the PX-891)

`byte[1]` = phase (`15` start / `16` read / `17` stop), `byte[2]` = type (`00` DVD PI/PIF,
`01` CD C1/C2, `10` jitter/beta). CD read `EA 16 01` → 26 B; DVD `EA 16 00` → 52 B.
(OptiScan's Plextor path instead uses the `0xE9`/`0xEB`/`0xD8` command set — see
`ScsiDrive.QCheck.cpp`.)

## `0xF3` / `0x3B`-`0x3C` — new-drive errc & Pioneer

| Sub-cmd | Function | Engine |
|---|---|---|
| `F3 0E A1` | errc init — byte[8] `00`=CD / `10`=DVD / `02`=BD | Lite-On/LG (new) |
| `F3 0E A2` | errc block read | Lite-On/LG (new) |
| `3B 02 E1` | arm error scan (WRITE BUFFER, id `0xE1`) | Pioneer |
| `3C 02 E1` | read error counters (READ BUFFER, id `0xE1`) | Pioneer |

## Reusable lesson

Never issue a single data-in transfer of `0x10000` bytes or more on ATAPI: the 16-bit
transfer-length field wraps, and the drive returns GOOD with 0 bytes. Chunk `<= 0xFFFE`
(OptiScan uses 16 sectors) and trust the returned transfer length.
