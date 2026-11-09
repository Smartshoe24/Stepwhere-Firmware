# Recordings 2026-10-31 (bring-up v0.3, right shoe, Rev11 board)

| File | Content | Truth | Counter |
|---|---|---|---|
| `walk1_s1_still.csv` | board lying still, 16 min | 0 steps | 0 |
| `walk1_s2_walk.csv` | putting the shoe on (10-95 s), walk 12:37-12:41 (file time ~100-225 s), standing/still after | "more than 100" steps (not counted exactly); 61 clear strides = 122 steps | 116 (v0.4), 132 (v0.5) |

File time runs ~4 % slow (sensor ODR is ~52 Hz, samples are stamped 20 ms apart).
Stride period ~1.4 s; the walk has pauses of 3-6 s every 5-6 strides (turning around?).
| `walk2_s1_walk100.csv` | walk 100 steps in a room 13:06-13:08 (file ~71-148 s, turns every ~6 strides), 20 s still, then leg moved in the air (~190-215 s) | 100 steps, 0 for the leg movement | 86 (v0.4, = board), 94 (v0.5) |

walk2: 44 clear strides + 5 turns with no clear swing = 49-50 strides. v0.5 counts
the 4 turns inside the walk; the first stride and the turn before the walk is
confirmed (~71-75 s) are still lost. Originally uploaded to the
`Hardware--Stepwhere` branch as `20261031_131121_walk2_s1.csv`.
| `sit1_walk100_taps_jiggle_walk102.csv` | 13:25-13:27 walk 100 (file ~65-185 s), sitting foot taps 13:27-13:30 (~185-380 s), sitting jiggling the shoe in the air 13:30-13:33 (~380-560 s), walk 102 13:33-13:35 (~560-700 s), still after | 100 / 0 / 0 / 102 | v0.5 board: 192 total; per part 94 / 0 / 0 / 98 |

The jiggle part has 7 bursts that pass the single-stride checks; only the
rhythm check kept them at 0. Watch this margin when tuning.
