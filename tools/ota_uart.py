#!/usr/bin/env python3
"""
ota_uart.py - Serial terminal + .ota uploader for the OTA test app (macOS, Linux, Windows).

  python3 tools/ota_uart.py ports
  python3 tools/ota_uart.py monitor  --port /dev/cu.usbserial-XXXX
  python3 tools/ota_uart.py upload   --port /dev/cu.usbserial-XXXX testapp_v1.1.ota --reset

monitor : shows the board output; keys you type are sent to the board
          (i, u, r, e, h). Quit with Ctrl-C.
upload  : sends 'u', transfers the file by XMODEM-1K, shows the result,
          optionally sends 'r' (--reset), then keeps monitoring.

Requires: pip3 install pyserial
"""
from __future__ import annotations

import argparse
import os
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("This tool needs pyserial:  pip3 install pyserial", file=sys.stderr)
    sys.exit(1)

SOH, STX, EOT, ACK, NAK, CAN = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18
BLOCK = 1024


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def safe_read(ser) -> bytes:
    try:
        return ser.read(ser.in_waiting or 1)
    except serial.SerialException:
        print("\n[serial port disconnected]")
        sys.exit(1)


def wait_for_text(ser, text: bytes, timeout: float, echo: bool = True) -> bool:
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = safe_read(ser)
        if chunk:
            buf += chunk
            if echo:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            if text in buf:
                return True
    return False


def xmodem_send(ser, data: bytes, progress=True) -> bool:
    # Wait for the receiver's 'C' (CRC mode request).
    end = time.time() + 60
    while time.time() < end:
        b = ser.read(1)
        if b == b"C":
            break
        if b and b[0] == CAN:
            return False
    else:
        print("\nno XMODEM start from the board")
        return False

    blocks = [data[i:i + BLOCK] for i in range(0, len(data), BLOCK)]
    for n, chunk in enumerate(blocks, start=1):
        chunk = chunk.ljust(BLOCK, b"\x1a")
        crc = crc16_xmodem(chunk)
        pkt = bytes([STX, n & 0xFF, 0xFF - (n & 0xFF)]) + chunk + bytes([crc >> 8, crc & 0xFF])
        for attempt in range(10):
            ser.reset_input_buffer()
            ser.write(pkt)
            reply = None
            t_end = time.time() + 10
            while time.time() < t_end:
                b = ser.read(1)
                if b and b[0] in (ACK, NAK, CAN):
                    reply = b[0]
                    break
            if reply == ACK:
                break
            if reply == CAN:
                print("\nboard cancelled the transfer")
                return False
        else:
            print(f"\nblock {n} failed 10 times")
            return False
        if progress:
            pct = 100 * n // len(blocks)
            sys.stdout.write(f"\r  sending: {n}/{len(blocks)} blocks ({pct}%)")
            sys.stdout.flush()
    for _ in range(10):
        ser.write(bytes([EOT]))
        b = ser.read(1)
        if b and b[0] == ACK:
            print()
            return True
    print("\nno ACK for end of transfer")
    return False


def monitor(ser) -> None:
    """Print board output; forward typed keys. Ctrl-C quits."""
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                chunk = ser.read(ser.in_waiting or 1)
            except serial.SerialException:
                print("\n[serial port closed]")
                stop.set()
                return
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    print("[monitor: type i/u/r/e/h, Ctrl-C to quit]")
    try:
        if os.name == "nt":
            import msvcrt
            while not stop.is_set():
                if msvcrt.kbhit():
                    ser.write(msvcrt.getch())
                time.sleep(0.01)
        else:
            import select
            import termios
            import tty
            fd = sys.stdin.fileno()
            old = termios.tcgetattr(fd)
            try:
                tty.setcbreak(fd)
                while not stop.is_set():
                    r, _, _ = select.select([sys.stdin], [], [], 0.1)
                    if r:
                        ch = os.read(fd, 1)
                        if ch == b"\x03":
                            break
                        ser.write(ch)
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, old)
    except KeyboardInterrupt:
        pass
    stop.set()
    print("\n[monitor stopped]")


def open_port(port: str):
    try:
        return serial.Serial(port, 115200, timeout=0.2)
    except serial.SerialException as e:
        print(f"cannot open {port}: {e}", file=sys.stderr)
        print("Close any other program using the port. Run 'ports' to list ports.", file=sys.stderr)
        sys.exit(1)


def cmd_ports(_args) -> None:
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("no serial ports found; is the USB-TTL adapter plugged in?")
    for p in ports:
        print(f"{p.device:40s} {p.description}")
    if sys.platform == "darwin":
        print("\nOn a Mac use the /dev/cu.* name (not /dev/tty.*).")


def cmd_monitor(args) -> None:
    monitor(open_port(args.port))


def cmd_upload(args) -> None:
    with open(args.file, "rb") as f:
        data = f.read()
    if data[:4] != b"SWFW":
        print(f"{args.file} is not a .ota file (no SWFW header)", file=sys.stderr)
        sys.exit(1)
    ser = open_port(args.port)
    ser.reset_input_buffer()
    ser.write(b"u")
    print(f"uploading {args.file} ({len(data)} bytes)")
    if not wait_for_text(ser, b"Ready.", 15):
        print("\nthe board did not answer 'u'. Is the OTA test app running?")
        sys.exit(1)
    print()
    ok = xmodem_send(ser, data)
    print("transfer complete" if ok else "transfer FAILED")
    end = time.time() + 5           # show the board's verdict
    while time.time() < end:
        chunk = safe_read(ser)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
            if b"PENDING" in chunk or b"REJECTED" in chunk or b"failed" in chunk:
                time.sleep(0.3)
                sys.stdout.write(safe_read(ser).decode("utf-8", "replace"))
                break
    if ok and args.reset:
        print("\n[sending r: reset to install]")
        ser.write(b"r")
    if not args.no_monitor:
        monitor(ser)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports", help="list serial ports").set_defaults(func=cmd_ports)
    m = sub.add_parser("monitor", help="show board output and send keys")
    m.add_argument("--port", required=True)
    m.set_defaults(func=cmd_monitor)
    u = sub.add_parser("upload", help="send a .ota file to the test app")
    u.add_argument("--port", required=True)
    u.add_argument("file")
    u.add_argument("--reset", action="store_true", help="send 'r' after a good upload")
    u.add_argument("--no-monitor", action="store_true", help="exit after the upload")
    u.set_defaults(func=cmd_upload)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
