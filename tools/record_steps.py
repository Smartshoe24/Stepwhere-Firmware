#!/usr/bin/env python3
"""
record_steps.py - Get motion recordings from tests/lis2dh12_bringup (macOS, Linux, Windows).

  In the shoe (no cable): the board records to its own flash after every
  power-up/reset. Afterwards, connect the cable and download:

    python3 tools/record_steps.py dump --port /dev/cu.usbserial-0001 --label day1
    python3 tools/record_steps.py dump --port /dev/cu.usbserial-0001 --label day1 --erase

  dump    downloads every session from the board's flash into
          recordings/<date>_<time>_<label>_s<N>.csv (one file per session)
          and prints the board's step count per session. --erase clears the
          board's log after a complete download.

  At the desk (cable connected), streaming live:

    python3 tools/record_steps.py live --port /dev/cu.usbserial-0001 --label test

  live    starts live streaming ('r'), saves to recordings/, Enter = marker,
          Ctrl-C stops.

Replay a file:  tests/motion_host/build/step_replay --decimate 2 FILE.csv

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


def stamp() -> str:
    return datetime.datetime.now().strftime("%Y%m%d_%H%M%S")


def open_port(port: str, baud: int):
    ser = serial.Serial(port, baud, timeout=0.2)
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def send_confirmed(ser, key: bytes, timeout: float = 3.0) -> bytes:
    """Send a key and answer 'y' if the board asks for confirmation.
    Returns bytes already read that belong to the command's output."""
    ser.write(key)
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += ser.read(ser.in_waiting or 1)
        i = buf.find(b"#send y to confirm")
        if i >= 0 and b"\n" in buf[i:]:
            ser.write(b"y")
            return buf[buf.index(b"\n", i) + 1:]
        if b"#dump begin" in buf or b"#eras" in buf or b"#rec odr" in buf:
            return buf    # board was not recording: no question asked
    return buf


def cmd_dump(args) -> None:
    ser = open_port(args.port, args.baud)
    os.makedirs(args.dir, exist_ok=True)
    prefix = os.path.join(args.dir, f"{stamp()}_{args.label}")
    print("Asking the board for its recordings ...")
    buf = send_confirmed(ser, b"d")
    f = None
    files = []
    session = None
    samples = 0
    complete = False
    last_rx = time.time()
    t0 = time.time()
    while True:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            last_rx = time.time()
        elif time.time() - last_rx > 10:
            print("\nNo data for 10 s; download stopped before '#dump end'.")
            break
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            if line.startswith("#rec "):
                session = line.split("session=")[-1]
                path = f"{prefix}_s{session}.csv"
                f = open(path, "w", newline="\n")
                f.write(f"# downloaded {datetime.datetime.now().isoformat(timespec='seconds')} label={args.label}\n")
                files.append([path, 0, 0, "?"])
                samples = 0
            if f:
                f.write(line + "\n")
                if line[0].isdigit():
                    samples += 1
                    files[-1][1] = samples
                elif line.startswith("#GAP"):
                    files[-1][2] += 1
            if line.startswith("#end session") and f:
                files[-1][3] = line.split("steps=")[-1]
                f.close()
                f = None
            if line.startswith("#dump end"):
                complete = True
                if "bad_pages=0" not in line:
                    print(f"\nWarning: {line}")
                break
            if line.startswith("#dump flash not available"):
                print("The board has no working flash log.")
                break
        if complete:
            break
        if time.time() - t0 > 1:
            sys.stdout.write(f"\r  {len(files)} session(s), {samples} samples in current one   ")
            sys.stdout.flush()
    if f:
        f.close()
    print()
    if not files:
        print("No sessions on the board." if complete else "Nothing received. Is lis2dh12_bringup v0.2 flashed?")
    for path, n, gaps, steps in files:
        print(f"  {path}: {n / 50 / 60:5.1f} min, {n} samples, {gaps} gaps, board counted {steps} steps")

    if args.erase:
        if complete:
            print("Erasing the board's log ...")
            send_confirmed(ser, b"e")
            end = time.time() + 600
            while time.time() < end:
                line = ser.readline().decode("utf-8", "replace").strip()
                if line.startswith("#erase ") and "/" not in line:
                    print(f"  {line}")
                    break
        else:
            print("Not erasing: the download was incomplete.")
    ser.write(b"n")   # start a new session so the board records again
    ser.close()
    print("The board is recording a new session again (or reset it).")


def cmd_live(args) -> None:
    os.makedirs(args.dir, exist_ok=True)
    out = args.out or os.path.join(args.dir, f"{stamp()}_{args.label}.csv")
    ser = open_port(args.port, args.baud)
    buf = send_confirmed(ser, b"r")
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


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, func, hlp in (("dump", cmd_dump, "download the sessions recorded in the board's flash"),
                            ("live", cmd_live, "stream live over the cable")):
        p = sub.add_parser(name, help=hlp)
        p.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbserial-0001")
        p.add_argument("--label", default="rec", help="short name for the files, e.g. day1, walk100")
        p.add_argument("--dir", default="recordings", help="output folder (default recordings/)")
        p.add_argument("--baud", type=int, default=115200)
        p.set_defaults(func=func)
    sub.choices["dump"].add_argument("--erase", action="store_true", help="erase the board's log after a full download")
    sub.choices["live"].add_argument("--out", help="output file")
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
