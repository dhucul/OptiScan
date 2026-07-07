# Pioneer firmware-update SCSI command reference

The vendor SCSI command set Pioneer's firmware updater uses to **reflash** a BDR-/DVR-series
drive, recovered by reverse-engineering `Updater.exe` from the **BDR-S13 FW 1.05EU** updater
(`BDR-S13JBK_UBK_EBK_CBK_FW105EU.exe`) with **Ghidra headless** decompilation.

> ⚠ **These are firmware-*write* commands, and OptiScan does NOT implement any of them.** This
> file is reference only. Unlike the read-only quality-scan commands in
> [`scsi-quality-scan-commands.md`](scsi-quality-scan-commands.md), these commands **reflash the
> drive** — a wrong CDB, or an interrupted flash, can permanently brick it. They are documented
> here so the flash opcodes are not confused with the (read-only) Pioneer quality-scan path, and
> as RE reference. **Not hardware-validated.**

## Don't confuse these with the Pioneer *quality* scan

Both paths use the same **`0x3B` WRITE BUFFER / `0x3C` READ BUFFER** opcodes — the *mode* and
*buffer id* fields are what separate a harmless quality read from a firmware write:

| Purpose | Opcode | Mode | Buffer id | OptiScan |
|---|---|---|---|---|
| **Quality scan** (C1/C2/TE) | `0x3B`/`0x3C` | `2` (data) | **`0xE1`** | **implemented** — `ScsiDrive.PioneerScan.cpp` (read-only) |
| **Firmware flash** | `0x3B` | `4`/`5`/`7` (download microcode) | `0xFF`/`0xFE`/`0xF0` | **not implemented** (this doc) |

If you ever touch `0x3B` for Pioneer in OptiScan, it must stay on **mode 2 / buffer id `0xE1`**.
The download-microcode modes below are the flash path.

## Transport

SPTI — `CreateFileA \\.\X:` + `DeviceIoControl` (`IOCTL_SCSI_PASS_THROUGH_DIRECT`), with a
WNASPI32 (ASPI) fallback. Same pass-through channel as `ScsiDrive::SendSCSIWithSense`.

## `0x3B` WRITE BUFFER CDB (the common flash builder)

Every flash command is a standard 10-byte WRITE BUFFER CDB, built by `Updater.exe` `FUN_004095E0`;
`mode` (CDB[1]) and `buffer id` (CDB[2]) select the operation:

```
[0] 0x3B                      WRITE BUFFER
[1] mode                      04/05/07 = download microcode (temp / +save / +offsets&save)
[2] buffer id                 0xFF control · 0xFE kernel · 0xF0 normal
[3..5] buffer offset (BE24)   running flash offset
[6..8] param list length (BE24, MSB forced 0 -> <= 0xFFFF)
[9] 0x00                      control
+ DATA-OUT payload; direction DATA_OUT when length != 0
```

## Command map

| Command | Updater fn | Opcode | mode | buf id | len | Purpose |
|---|---|---|---|---|---|---|
| Identify | `FUN_00408E60` | `0x12` INQUIRY | — | — | 96 | vendor/model/revision; compared to the image `ID` field |
| Spin-up / ready | `FUN_00403AB0` | `0x1B` START/STOP + poll | — | — | 0 | START UNIT, then wait ready |
| Wait ready | `FUN_004041D0` | poll (TUR / REQUEST SENSE) | — | — | — | waits on sense bytes `02/04/xx`, `06` |
| **Enter kernel mode** | `FUN_0040A060` | `0x3B` | **4** | **`0xFF`** | `0x100` | 256-B control block → drive reboots into bootloader; then `Sleep(3000)` |
| Verify kernel mode | `FUN_00404DD0` / `FUN_0040A4E0` | `0x12` INQUIRY | — | — | 96 | model reads `"PIONEER DVD-RW  DVR-"` (DVR-103 boot signature) and/or a 3-B rev field reads `"000"` |
| Flash **kernel** part | `FUN_0040A370` | `0x3B` | 7 | **`0xFE`** | chunks | the `0x11200`-byte kernel image (updater resource 131) |
| Flash **normal** part | `FUN_0040A3B0` | `0x3B` | 7 | **`0xF0`** | `0x8000` | the ~2 MB normal image (resource 132), 32 KB chunks |
| **Commit / re-flash** | `FUN_0040A1D0` | `0x3B` | **5** | **`0xFF`** | `0x100` | 256-B control block, **timeout 180 s** ("internal re-flashing") |
| Status poll | `FUN_00408E60` loop | `0x12` INQUIRY / `0x3C` READ BUFFER | — | — | — | waits for status bytes `02 / 04 / 12` = done |

### Kernel-mode-switch CDB

```
3B 04 FF 00 00 00 00 01 00 00   + 256-byte DATA-OUT payload
```

Mode 4 ("download microcode", temporary) to buffer id `0xFF` — the WRITE-BUFFER download-microcode
channel used as a *command* to reboot the drive into its bootloader. The 256-byte payload carries a
4-byte control field at payload offset `0x10` (`DAT_005ADA78..7B`, **computed at runtime** — not a
fixed constant we recovered). Matches the known Pioneer DVR/BDR scheme (cf. DVRFlash / DVRTool).

## Control flow (two paths)

A config-table gate (`DAT_005ADA48`, set from `DAT_005AE1C0[]` in `FUN_00403BB0`) selects the path.
The kernel-mode switch is independent — it runs whenever the drive is not already in boot mode.

- **Normal-only** (`gate != 0`): identify → guard → wait-ready → *switch if needed* → write full
  normal image (`0xF0`) → commit.
- **Combined kernel + normal** (`gate == 0`): additionally writes *most* of the normal image
  (`0xF0`, up to size − `0x10000`), then the kernel image (`0xFE`), then re-writes the *full* normal
  image (`0xF0`) before commit.

Pre-flash guards: `FUN_00403D80` = model/version match; `FUN_004041D0`/`FUN_00403AB0` =
TUR/START-UNIT polls; `FUN_004040D0` = "does the kernel need updating?" (INQUIRY revision vs the
kernel image's revision fields at image `+0x130`/`+0x150`).

## Honest limits (why this stays reference-only)

- **The per-command CDBs are byte-exact**, but a *safe, complete* flasher can't be built from this
  RE alone: the kernel-mode-switch payload's 4-byte control field is **runtime-computed** (unknown
  value), and the combined path's **double normal write** (partial, then full) was **not
  explained**.
- **Not hardware-validated** — recovered purely by static decompilation.
- OptiScan is a disc ripping/diagnostics tool, **not a firmware flasher**. It writes *discs*
  (Copy / Write / Erase), never drive firmware. Implementing the above would be a different tool
  (see the open-source DVRFlash/DVRTool for a real Pioneer flasher).

Full protocol write-up (identical findings, FirmwareStudio side):
`FirmwareStudio/docs/pioneer-firmware-update-protocol.md`.
