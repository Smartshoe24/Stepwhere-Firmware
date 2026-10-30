#!/usr/bin/env python3
"""
record_steps.py - Record accelerometer data from tests/lis2dh12_bringup (macOS, Linux, Windows).

  python3 tools/record_steps.py --port /dev/cu.usbserial-0001 --label walk100

Starts a recording on the board ('r'), saves every line to a CSV file and
shows a live status line. Press Enter to insert a marker (for example when
you start and stop walking). Press Ctrl-C to stop; the board gets 's'.

The file goes to recordings/<date>_<time>_<label>.csv unless --out is given.
Replay it with:  tests/motion_host/build/step_replay --decimate 2 FILE.csv

Requires: pip3 install pyserial
The port cannot be open in another program (screen, bench console) at the same time.
"""
from __future__ import annotations

import argparse
import datetime
import os
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("This tool needs pyserial:  pip3 install pyserial", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbserial-0001")
    ap.add_argument("--label", default="rec", help="short name for the file, e.g. walk100, sitting_taps")
    ap.add_argument("--out", help="output file (default recordings/<date>_<label>.csv)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    out = args.out
    if not out:
        os.makedirs("recordings", exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        out = os.path.join("recordings", f"{stamp}_{args.label}.csv")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(b"s")              # stop a recording that may still be running
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"r")

    state = {"samples": 0, "steps": 0, "marks": 0, "overruns": 0, "started": False, "stop": False}
    lock = threading.Lock()

    def keys() -> None:
        while not state["stop"]:
            try:
                sys.stdin.readline()
            except (EOFError, KeyboardInterrupt):
                return
            if state["stop"]:
                return
            with lock:
                ser.write(b"m")

    threading.Thread(target=keys, daemon=True).start()
    print(f"Recording to {out}")
    print("Press Enter to add a marker, Ctrl-C to stop.")

    t0 = time.time()
    last_status = 0.0
    buf = b""
    with open(out, "w", newline="\n") as f:
        f.write(f"# recorded {datetime.datetime.now().isoformat(timespec='seconds')} label={args.label}\n")
        try:
            while True:
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").strip()
                        if not line:
                            continue
                        if line.startswith("#rec"):
                            state["started"] = True
                        if not state["started"]:
                            continue
                        f.write(line + "\n")
                        if line[0].isdigit():
                            state["samples"] += 1
                        elif line.startswith("#steps "):
                            state["steps"] = int(line.split()[1])
                        elif line.startswith("#MARK"):
                            state["marks"] += 1
                            print(f"\n  {line}")
                        elif line.startswith("#OVERRUN"):
                            state["overruns"] += 1
                        elif line.startswith("#ERROR"):
                            print(f"\n  {line}")
                now = time.time()
                if now - last_status > 1.0:
                    last_status = now
                    if not state["started"] and now - t0 > 3:
                        print("\nNo '#rec' line from the board. Is lis2dh12_bringup flashed and the port right?")
                        t0 = now
                    sys.stdout.write(f"\r  {now - t0:6.0f} s  samples {state['samples']:7d}  "
                                     f"board steps {state['steps']:5d}  marks {state['marks']}  "
                                     f"overruns {state['overruns']}   ")
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
        finally:
            state["stop"] = True
            with lock:
                ser.write(b"s")
            end = time.time() + 1.0
            while time.time() < end:
                line = ser.readline().decode("utf-8", "replace").strip()
                if line:
                    f.write(line + "\n")
                    if line.startswith("#stop"):
                        break
            ser.close()
    print(f"\nSaved {state['samples']} samples ({state['samples'] / 50:.0f} s) to {out}")
    print(f"Board counted {state['steps']} steps. Replay: tests/motion_host/build/step_replay --decimate 2 {out}")


if __name__ == "__main__":
    main()
