# Hardware fixes for the next board revision (Rev12)

Collected from the Rev11 bring-up and the firmware review. Each item: what is
wrong on Rev11, the fix, and where it was found.

| # | Part / net | Rev11 | Fix for Rev12 | Source |
|---|---|---|---|---|
| H-1 | IC7 LIS2DH12 pin 5 ("Res") | Not connected (floating) | Connect pin 5 to GND. ST: the reserved pin should go to GND; re-check Table 2 of the LIS2DH12 datasheet when doing the layout | `docs/STEPS_DECISIONS.md` (open items); [ST community](https://community.st.com/mems-sensors-48/what-is-the-res-pin-for-in-the-lis2dh12-mems-accelerometer-83491) |
| H-2 | IC1 TXB0102 VCCB (pin 7) | No 3.3 V feed anywhere in the layout; floats at ~1.8 V. R38 (1 kΩ, not fitted) goes from VCCB to GND, so it is not the feed either | Connect VCCB to the MCU 3.3 V rail, add 100 nF from VCCB to GND, drop or re-purpose R38 | `docs/BRINGUP_REV11.md` T6–T8 (branch `bringup/rev11`) |

Rev11 boards: H-1 needs no rework unless the accelerometer shows stalls or
restarts (`[ACCEL] ... restarts N` in the minute report). H-2 is reworked
with a wire from VCCB to 3V3 (see the bring-up log).
