# Stepwhere QA Firmware — Tasks (v2)

Status: DRAFT v2, awaiting approval. No QA code before approval.
Branch `feature/qa-firmware`, draft PR. Only `stepwhere_qa/` changes.

- [ ] 1. Skeleton `stepwhere_qa/stepwhere_qa.ino`: header, includes (TinyGPSPlus, Wire,
  HardwareSerial, SPI), `#define`s (design §2), pins/objects, result model (§3),
  `setup()`/`loop()` stubs. _R0, C2, C6, C7_
- [ ] 2. Protocol output: CRC-16/CCITT-FALSE, JSON writer with escaping, UID / MCU info,
  `QA_HELLO`, `QA_PROMPT`, `QA_JSON`, `QA_RESULT`, repeat timer. _R7_
- [ ] 3. Host test `stepwhere_qa/tests/qa_proto_test.c` + `run.sh`: CRC check value
  "123456789" → 29B1, escaping of `"`, `\`, control bytes and bytes ≥ 0x7F, CRC of a
  sample line. Same functions as the sketch (copied between markers, `run.sh` checks
  that the copies are identical). _A2_
- [ ] 4. Power start (PWRKEY HIGH, LDO on, TTFF clock). _R0.1_
- [ ] 5. Accelerometer: I2C helpers, ACC_I2C (SA0 both ways), ACC_WHOAMI, ACC_XYZ,
  ACC_SELFTEST, ACC_INT1/INT2 (pull-up/pull-down probe, latched IA1/IA2), ACC_TAP
  behind `QA_TAP_TEST`. _R1_
- [ ] 6. FLASH_ID (0xAB, 0x9F, read-only). _R2_
- [ ] 7. BAT_ADC (VREFINT calibration, divider, saturation). _R3_
- [ ] 8. GPS: pump, GSV readers, GPS_UART, GPS_FIX in the background with the final wait. _R4_
- [ ] 9. GSM: AT helper, power-on with AT check, steps 1–9, skip rules, `[NET]` block. _R5_
- [ ] 10. Report table and OVERALL rule. _R6_
- [ ] 11. Interactive mode (letter + Enter, pass-throughs with timeouts, RX pull-up). _R8_
- [ ] 12. `stepwhere_qa/README.md`. _R9_
- [ ] 13. Verification: arduino-cli compile with STM32duino 2.12.0 / GENERIC_G070CBTX,
  warnings all, 0 warnings from the sketch; report the size; run the host test;
  self-review; `git diff --stat` shows only `stepwhere_qa/`. _A1–A3_
- [ ] 14. Commit and push to `feature/qa-firmware`; the draft PR updates.

Not in this work: bench test on hardware (A4, by the team), the agent itself,
anything outside `stepwhere_qa/`.
