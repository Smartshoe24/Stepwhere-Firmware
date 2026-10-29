# Stepwhere OTA design

Status: **draft, phase 1–2 groundwork**. Binary formats and addresses here are
implemented in `libraries/StepwhereOTA/src/ota_layout.h` and
`tools/ota_tool.py`. Change them only before the first devices ship.

## Hardware

| Part | Role |
|---|---|
| STM32G070CBT6 | 128 KB flash (single bank, 2 KB pages), 36 KB RAM |
| W25Q128FV (SOIC-8, IC8) | 16 MB SPI NOR, 4 KB sectors, 64 KB blocks, 256 B pages |
| Neoway N706B | LTE Cat-1 modem on USART2, downloads the image over HTTP(S) |
| 3.7 V 1500 mAh Li-ion | Update only when battery is high enough |

## W25Q128 wiring (from schematic)

| W25Q pin | Signal | STM32 pin | Notes |
|---|---|---|---|
| 1 | /CS | PC6 (pin 30) | GPIO, active low |
| 2 | DO | PA6 (pin 17) | SPI1_MISO |
| 5 | DI | PA7 (pin 18) | SPI1_MOSI |
| 6 | CLK | PA5 (pin 16) | SPI1_SCK |
| 3 | /WP | PC7 (pin 31) | drive HIGH in app and bootloader |
| 7 | /HOLD | PB12 (pin 24) | drive HIGH in app and bootloader |
| 8 | VCC | 3.3 V LDO | which rail (always-on or LDO_EN) is checked by the self-test |

## Update flow

```
GitHub tag vX.Y.Z -> CI builds app at 0x08004000 -> ota_tool.py pack + sign
  -> firmware .ota + manifest.json uploaded to update server
  -> device app polls manifest, downloads .ota via N706B into W25Q download slot
  -> app verifies header, SHA-256 and signature -> state PENDING -> reset
  -> bootloader backs up current app, installs new one, boots it (TRIAL)
  -> new app self-tests and calls ota_confirm() -> IDLE
  -> no confirm after OTA_MAX_TRIAL_BOOTS boots -> bootloader restores backup
```

## Internal flash

| Region | Address | Size |
|---|---|---|
| Bootloader | `0x08000000` | 16 KB |
| Application | `0x08004000` | 112 KB (current build: ~72 KB) |

The app does not store a header in internal flash. The bootloader keeps the
installed version, size and SHA-256 in the boot metadata instead.

## External flash (W25Q128)

| Region | Offset | Size | Use |
|---|---|---|---|
| Metadata A | `0x000000` | 4 KB | boot state, copy A |
| Metadata B | `0x001000` | 4 KB | boot state, copy B |
| (unused) | `0x002000` | 56 KB | reserved for OTA growth |
| Download slot | `0x010000` | 128 KB | image being downloaded |
| Backup slot | `0x030000` | 128 KB | previous app, for rollback |
| Factory slot | `0x050000` | 128 KB | known-good image, last resort |
| App data | `0x070000` | ~15.5 MB | GPS logs, config, … |
| Self-test scratch | `0xFFF000` | 4 KB | used only by `tests/flash_selftest` |

Slots are 64 KB aligned so they erase with two 64 KB block erases
(~0.3 s typical, 4 s worst case per datasheet).

Metadata is written alternately to A and B with an increasing sequence
number. On boot the valid record (magic + CRC) with the highest sequence
number wins, so a power cut during a metadata write cannot lose the state.

## Image format (`.ota`)

`[256-byte header][application .bin]`, little-endian.

| Offset | Field | Notes |
|---|---|---|
| 0 | magic | `"SWFW"` (0x57465753) |
| 4 | hdr_version, hdr_size | 1, 256 |
| 8 | fw_version | `0x00MMmmpp`, v1.2.3 = 0x00010203 |
| 12 | image_size | payload bytes |
| 16 | load_addr | must be `0x08004000` |
| 20 | hw_id | `0x00010001` = GSM PCB rev 1; wrong board is refused |
| 24 | flags | bit 0 = signed |
| 28 | build_time | Unix time |
| 32 | payload_sha256 | SHA-256 of the payload |
| 64 | git_sha1 | commit it was built from |
| 84 | reserved | zero |
| 128 | signature | ECDSA P-256, raw r‖s, over SHA-256 of bytes 0–127 |
| 192 | reserved | zero |
| 252 | hdr_crc32 | CRC-32 (zlib/IEEE) of bytes 0–251 |

The signature covers the payload hash, so it protects the whole image.
The CRC only catches corruption cheaply before the slow checks.

## Boot states

`IDLE → PENDING → BACKING_UP → INSTALLING → TRIAL → IDLE`, plus
`ROLLING_BACK`. Each state is saved before its step starts, so after a reset
the bootloader repeats the interrupted step. The download slot is not erased
until the new app is confirmed.

## Findings from the STM32duino core (2.12.0)

* `--build-property build.flash_offset=0x4000` moves the app to
  `0x08004000`: the linker script uses `LD_FLASH_OFFSET`, and the app area
  length becomes 128 KB − 16 KB automatically.
* The core's `SystemInit()` for STM32G0 does **not** set `SCB->VTOR` (the CMSIS
  `USER_VECT_TAB_ADDRESS` switch is off). The **bootloader must set
  `SCB->VTOR = 0x08004000` before jumping** to the app.
* Core 3.0.0 changed `HardwareSerial(rx, tx)`; the current firmware only
  builds on the 2.x core.

## Signing keys

* `python tools/ota_tool.py keygen --out keys/` creates the key pair once.
* `keys/` is git-ignored. The private key goes into the GitHub secret
  `OTA_SIGNING_KEY` and an offline backup. **If it is lost, shipped devices
  can no longer be updated.**
* `ota_public_key.h` (public key) is compiled into the bootloader.

## Open items

* Confirm whether the flash VCC is the always-on 3.3 V rail or the
  LDO_EN-switched one (the self-test reports it). If it is switched, the
  bootloader must enable LDO_EN before touching the flash.
* Confirm board selection: G070CBTx (BOM) vs G071CB (comment in stepwhere.ino).
* Bootloader (LL drivers, SHA-256, ECDSA verify, state machine).
* App-side OTA client for the N706B, GitHub Actions workflow, hosting.
