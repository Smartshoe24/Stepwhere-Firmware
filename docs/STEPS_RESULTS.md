# Step counter results

Measured step counts, kept apart from the tuning notes in `STEPS_DECISIONS.md`.

## Rules for held-out tests (agreed 2026-11-03)

1. The counter under test is tag **`steps-eval-1`** (commit `425c354`).
   `libraries/StepwhereMotion/src/step_counter.c/.h` and
   `step_counter_default_config()` are unchanged since then; check with
   `git diff steps-eval-1 -- libraries/StepwhereMotion/src/step_counter.*`
   before reporting.
2. New recordings are reported **with that version first**: truth, count and
   error, in the table below. No thresholds are changed before that report.
3. A recording used to tune a threshold is marked "training" from then on.
   Accuracy claims use only held-out recordings.
4. Every recording is replayed two ways, because the firmware paths differ:
   * `step_replay --decimate 2 file.csv`: bring-up path (50 Hz, pairs averaged)
   * `main_path_replay file.csv 0` and `... 1`: main firmware path (native
     25 Hz, 10-bit, FIFO, 500 ms reads)
   All three are in `tests/motion_host/build/` after
   `sh tests/motion_host/run_tests.sh`.
5. Targets (S-10, proposed): walking within ±3 % over 100+ steps; 0 steps in
   5 min sitting; < 10 false steps per school hour; vehicle ride: target not
   set yet (proposal: 0).

## Results with `steps-eval-1`

| Recording | Truth | Status | Bring-up path | Main path ph0 / ph1 | Error (worst) |
|---|---|---|---|---|---|
| 2026-10-31 walk1_s1_still | 0 | training (S-14) | 0 | 0 / 0 | 0 |
| 2026-10-31 walk1_s2_walk | ~122 (61 clear strides; "more than 100") | training (S-14, S-15) | 132 | 134 / 124 | not exact |
| 2026-10-31 walk2_s1_walk100 | 100 | training (S-16) | 94 | 96 / 96 | −6 % |
| 2026-10-31 sit1 walk 100 / taps / jiggle / walk 102 | 202, sitting parts 0 | recorded after the last tuning commit, not tuned on; same wearer, room and day, so **not independent** | 192 (94 / 0 / 0 / 98 on the board) | 196 / 194 | −5 % |

None of these is a clean held-out test. Planned held-out recordings (by
Diksha): straight 200 steps counted exactly, stairs, another wearer, a car or
bus ride. Add one row per recording here, with the commit used.

## Item 8: main firmware counted 0 steps during a walk (2026-11-03 log)

The 17-minute log on `main` (`Stepwhere-01-tty.usbserial-...log`, firmware
v1.2-lis2dh12) shows `steps since boot 0` while someone walked with the
device, with 24.5 Hz samples, 0 overruns and 0 I2C errors. How the device was
carried is not known yet.

Firmware-side check: the main firmware path reproduces the bring-up counts
on every recording (table above, columns "Main path"). Also tested with 16 mg
quantisation (10-bit at ±8 g) and with averaged pairs: 200 / 132 / 94. The
sensor rate, resolution, FIFO handling and counter configuration are
therefore **not** the cause.

Remaining explanations, to be told apart with the `[STEPS]` debug line
(`STEPS_DEBUG`, firmware v1.2-lis2dh12.2):

* **Not worn on the foot** (hand, pocket, bag). The counter only accepts the
  foot pattern by design (S-05/S-06): still stance, then a 200–900 ms swing
  burst with a peak of at least 450 mg. Expected debug line: low `still %`,
  or many bursts "below 450 mg" with few candidates; max motion under
  450 mg, or rejects "long" / "no-stance".
* **Worn on the foot, but live 25 Hz data differs** from the decimated
  recordings (for example the sensor's own filter at 25 Hz). Expected: many
  candidates rejected for "energy" or "short", max motion near 450 mg.

Next test: main firmware v1.2-lis2dh12.2, two walks of 100 steps, one with
the board taped in the shoe heel and one carried in the hand. Report the
`[STEPS]` lines here. A threshold change is proposed only after that.

Reference: the same bookkeeping (`libraries/StepwhereMotion/src/step_debug.h`,
shared with the firmware) over the whole recording, main path, phase 0.
Reproduce with `tests/motion_host/build/main_path_replay <file> 0` (last line).
`run_tests.sh` checks that these numbers agree with the counter's own totals.

| Recording | Samples | Still % | Bursts | Candidates | Passed | Rejected (short / long / energy / no-stance) | Steps | Max \|a\| | Max motion |
|---|---|---|---|---|---|---|---|---|---|
| walk2_s1_walk100 | 8537 | 84 % | 201 | 116 | 49 | 67 (8 / 2 / 57 / 0) | 96 | 4393 mg | 1786 mg |
| walk1_s1_still | 24150 | 99 % | 2 | 0 | 0 | 0 | 0 | 1272 mg | 263 mg |
| sit1_walk100_taps_jiggle_walk102 | 21137 | 84 % | 657 | 228 | 106 | 122 (19 / 1 / 102 / 0) | 196 | 5916 mg | 3011 mg |

A real walk in the shoe therefore shows candidates passing at about one per
stride, plus rejected "energy" bursts (the push-off between strides).
