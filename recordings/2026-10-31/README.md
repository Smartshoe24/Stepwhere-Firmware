# Recordings 2026-10-31 (bring-up v0.3, right shoe, Rev11 board)

| File | Content | Truth | Counter |
|---|---|---|---|
| `walk1_s1_still.csv` | board lying still, 16 min | 0 steps | 0 |
| `walk1_s2_walk.csv` | putting the shoe on (10-95 s), walk 12:37-12:41 (file time ~100-225 s), standing/still after | "more than 100" steps (not counted exactly); 61 clear strides = 122 steps | 116 (v0.4 counter) |

File time runs ~4 % slow (sensor ODR is ~52 Hz, samples are stamped 20 ms apart).
Stride period ~1.4 s; the walk has pauses of 3-6 s every 5-6 strides (turning around?).
