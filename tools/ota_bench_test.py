#!/usr/bin/env python3
"""
ota_bench_test.py - Runs the whole OTA bench test on one board, automatically.

Board: flashed once with ota_test_kit.bin (bootloader + OTA test app) at 0x08000000.
Run:   python3 ota_bench_test.py --port /dev/cu.usbserial-0001 --key ~/stepwhere-keys/ota_signing_key.pem

It signs the test apps with your key, uploads them one by one, presses the
keys, checks every reply against the expected result, and writes a results
file. You only act when it asks (one battery pull; one optional ST-LINK step).
Needs: pip3 install pyserial cryptography
"""
from __future__ import annotations

import argparse
import http.client
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import serial
except ImportError:
    sys.exit("needs pyserial:  python3 -m pip install --user pyserial")

import ota_tool as t          # noqa: E402
from ota_uart import xmodem_send  # noqa: E402

PROD_KEY_FP = "59C07275"
BOOT_VERSION = "0.2.2"


class Fail(Exception):
    pass


class Board:
    def __init__(self, port: str, log):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.log = log
        self.buf = ""          # text not yet consumed by expect()

    def _pump(self):
        try:
            data = self.ser.read(self.ser.in_waiting or 1)
        except serial.SerialException as e:
            raise Fail(f"serial port error: {e}")
        if data:
            text = data.decode("utf-8", "replace")
            self.log.write(text)
            self.log.flush()
            self.buf += text
            if len(self.buf) > 200000:
                self.buf = self.buf[-100000:]

    def send(self, key: str):
        self.log.write(f"\n<<< sent '{key}' >>>\n")
        self.ser.write(key.encode())

    def drain(self, secs=0.3):
        end = time.time() + secs
        while time.time() < end:
            self._pump()
        self.buf = ""

    def expect(self, *patterns, timeout=30, fail_on=()):
        """Wait for patterns in order. Returns the text seen."""
        seen = ""
        for pat in patterns:
            end = time.time() + timeout
            while True:
                i = self.buf.find(pat)
                for bad in fail_on:
                    j = self.buf.find(bad)
                    if j >= 0 and (i < 0 or j < i):
                        raise Fail(f"got '{bad}' (expected '{pat}')")
                if i >= 0:
                    seen += self.buf[:i + len(pat)]
                    self.buf = self.buf[i + len(pat):]
                    break
                if time.time() > end:
                    raise Fail(f"timeout waiting for '{pat}'")
                self._pump()
        return seen

    def wait_idle_app(self, version: str, timeout=60):
        self.expect(f"OTA TEST APP v{version}", "watchdog: off", "Commands:", timeout=timeout)

    def upload(self, path: str, expect_ok: bool, reason: str = ""):
        with open(path, "rb") as f:
            data = f.read()
        self.drain()
        self.send("u")
        self.expect("Ready.", timeout=20)
        self.log.write(f"\n<<< XMODEM {os.path.basename(path)} ({len(data)} bytes) >>>\n")
        if not xmodem_send(self.ser, data, progress=False):
            raise Fail("XMODEM transfer failed")
        if expect_ok:
            self.expect("Image OK", "Marked PENDING", timeout=20, fail_on=("REJECTED",))
        else:
            self.expect(f"REJECTED: {reason}", timeout=20, fail_on=("Image OK",))


# --------------------------------------------------------------- the tests ---

def t_alive(b: Board, ctx):
    b.drain()
    b.send("h")
    try:
        b.expect("Commands:", timeout=5)
    except Fail:
        print("    board not answering: press the board's reset (or power-cycle it) now ...")
        b.expect("Commands:", timeout=60)
    return "test app answers"


def t_selftest(b: Board, ctx):
    b.drain()
    b.send("s")
    b.expect("JEDEC ID: ", timeout=10)
    out = "JEDEC ID: " + b.expect("RESULT:", timeout=30)
    tail = b.expect("\n", timeout=5)
    jid = re.search(r"JEDEC ID: ([0-9A-F]+ [0-9A-F]+)", out)
    if "ALL TESTS PASSED" not in tail:
        raise Fail("self-test reported FAILED")
    return f"JEDEC {jid.group(1) if jid else '?'}"


def t_first_boot(b: Board, ctx):
    b.drain()
    b.send("e")
    b.expect("Metadata erased", timeout=10)
    b.send("r")
    b.expect(f"signing key {PROD_KEY_FP}", f"bootloader v{BOOT_VERSION}",
                   "no metadata (first boot)", "starting application", timeout=20,
                   fail_on=("WARNING: development",))
    ctx["app"] = re.findall(r"OTA TEST APP v([\d.]+)", b.expect("watchdog: off", "Commands:", timeout=20))
    return f"bootloader v{BOOT_VERSION}, key {PROD_KEY_FP}, watchdog off"


def t_good_update(b: Board, ctx):
    b.upload(ctx["f"]["v1.1"], True)
    b.send("r")
    out = b.expect("watchdog on", "signature ok", "backing up current app",
                   "installed v1.1.0, trial boot 1 of 3", timeout=40,
                   fail_on=("bad signature", "not signed", "SHA-256 mismatch"))
    ms = re.search(r"signature ok \((\d+) ms\)", out)
    b.expect("OTA TEST APP v1.1.0", "RUNNING (trial boot)", "New firmware confirmed", timeout=30)
    b.wait_idle_app("1.1.0")
    return f"installed + confirmed, bootloader signature check {ms.group(1) if ms else '?'} ms"


def t_crash_rollback(b: Board, ctx):
    b.upload(ctx["f"]["v1.2"], True)
    b.send("r")
    b.expect("installed v1.2.0, trial boot 1 of 3", "NO-CONFIRM build", timeout=40)
    b.expect("trial boot 2 of 3", "trial boot 3 of 3", timeout=60)
    b.expect("not confirmed after 3 boots, rolling back", "restored v1.1.0 from backup slot",
             "update work done, restarting", timeout=60)
    b.wait_idle_app("1.1.0")
    return "3 failed boots -> rolled back to v1.1.0"


def t_hang_rollback(b: Board, ctx):
    b.upload(ctx["f"]["v1.4"], True)
    b.send("r")
    b.expect("installed v1.4.0, trial boot 1 of 3", "Hanging now", timeout=40)
    t0 = time.time()
    b.expect("trial boot 2 of 3", timeout=40)
    wd = time.time() - t0
    b.expect("trial boot 3 of 3", timeout=40)
    b.expect("rolling back", "restored v1.1.0 from backup slot", timeout=45)
    b.wait_idle_app("1.1.0")
    return f"watchdog reset after {wd:.0f} s (expected ~20), rolled back to v1.1.0"


def reject_case(b: Board, path: str, reason: str):
    b.upload(path, False, reason)
    b.drain(0.5)
    b.send("f")
    b.expect("PENDING WITHOUT checks", timeout=10)
    b.send("r")
    b.expect(f"download slot: {reason}", "update work done, restarting", timeout=30,
             fail_on=("trial boot 1 of 3",))
    b.wait_idle_app("1.1.0")
    return f"app and bootloader both refused ({reason})"


def t_reject_corrupt(b, ctx):
    return reject_case(b, ctx["f"]["v1.3"], "SHA-256 mismatch")


def t_reject_devkey(b, ctx):
    return reject_case(b, ctx["f"]["v1.5"], "bad signature")


def t_reject_unsigned(b, ctx):
    return reject_case(b, ctx["f"]["v1.6"], "not signed")


def t_power_cut(b: Board, ctx):
    for attempt in range(1, 4):
        b.upload(ctx["f"]["v1.1"], True)
        print("\n" + "!" * 64)
        print("  GET READY: when you see  >>> PULL THE BATTERY NOW <<<")
        print("  unplug the battery, wait 2 seconds, plug it back in.")
        print("!" * 64)
        time.sleep(2)
        b.send("r")
        b.expect("backing up current app", timeout=30)
        print("\n  >>> PULL THE BATTERY NOW <<<\n", flush=True)
        try:
            b.expect("Stepwhere bootloader", timeout=90)
            b.expect("state ", timeout=10)
            st = b.expect("\n", timeout=5)
        except Fail:
            raise Fail("board did not come back within 90 s after the cut")
        if "PENDING" in st or "BACKING_UP" in st or "INSTALLING" in st:
            b.expect("New firmware confirmed", timeout=60)
            b.wait_idle_app("1.1.0")
            return f"resumed from state {st.strip().split(',')[0]}, finished and confirmed (try {attempt})"
        print(f"  the install finished before the battery was pulled (state {st.strip()}).")
        b.wait_idle_app("1.1.0", timeout=60)
        if attempt < 3:
            print("  trying again: pull it a little faster this time.")
    return "SKIP: install always finished before the cut"


def ask(prompt: str) -> str:
    try:
        return input(prompt)
    except EOFError:
        return ""


def t_cable_reflash(b: Board, ctx):
    ans = ask("\n  Cable-reflash test needs the ST-LINK. Do it now? [y/N] ").strip().lower()
    if ans != "y":
        return "SKIP: not run"
    b.ser.close()
    print("  1. In STM32CubeProgrammer flash ota_test_kit.bin at 0x08000000 (NO chip erase).")
    print("  2. Click Disconnect.")
    ask("  3. Press Enter here when done ... ")
    b.ser.open()
    b.drain()
    t_alive(b, ctx)
    b.upload(ctx["f"]["v1.1"], True)
    b.send("r")
    b.expect("app differs from installed v1.1.0", timeout=30)
    b.expect("New firmware confirmed", timeout=40)
    b.wait_idle_app("1.1.0")
    b.drain()
    b.send("i")
    b.expect("backup   slot: ", timeout=10)
    line = b.expect("\n", timeout=5)
    if "v0.0.0" not in line:
        raise Fail(f"backup should be labelled v0.0.0, got: {line.strip()}")
    return "cable-flashed app detected, backup labelled v0.0.0"


def t_server(b, ctx):
    root = tempfile.mkdtemp(prefix="fwsrv_")
    port = 18765
    srv = None
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, "fw_server.py"), "--root", root, "publish",
                            "--bin", ctx["bins"]["v1.1"], "--version", "1.1.0", "--key", ctx["key"]],
                           capture_output=True, text=True)
        if r.returncode:
            raise Fail("publish failed: " + (r.stderr or r.stdout).strip())
        srv = subprocess.Popen([sys.executable, os.path.join(HERE, "fw_server.py"), "--root", root,
                                "serve", "--bind", "127.0.0.1", "--port", str(port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.5)

        def get(path, rng=None):
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            c.request("GET", path, headers={"Range": rng} if rng else {})
            resp = c.getresponse()
            return resp.status, resp.read()

        st, man = get("/manifest.json")
        if st != 200 or b'"1.1.0"' not in man:
            raise Fail("manifest not served")
        s1, p1 = get("/releases/1.1.0/stepwhere.ota", "bytes=0-1023")
        s2, p2 = get("/releases/1.1.0/stepwhere.ota", "bytes=1024-")
        with open(os.path.join(root, "releases/1.1.0/stepwhere.ota"), "rb") as f:
            whole = f.read()
        if (s1, s2) != (206, 206) or p1 + p2 != whole:
            raise Fail("resumed download does not match")
        t.check_image(whole, pubkey=os.path.join(HERE, "production_public_key.pem"), require_signature=True)
        return f"publish + serve OK, resume OK, signature OK ({len(whole)} bytes)"
    finally:
        if srv:
            srv.terminate()
        shutil.rmtree(root, ignore_errors=True)


def t_modem(b: Board, ctx):
    b.drain()
    b.send("n")
    b.expect("N706B PROBE START", timeout=10)
    out = b.expect("N706B PROBE END", timeout=300)
    notes = []
    notes.append("AT ok" if "AT+CGMR" in out and "OK" in out else "modem silent")
    notes.append("SIM ready" if "READY" in out else "SIM NOT ready")
    notes.append("IP ok" if re.search(r"\+XIIC:\s*1,\s*\d+\.\d+", out) else "no IP")
    for name, mark in (("A", "Method A"), ("B", "Method B"), ("C", "Method C")):
        i = out.find(mark)
        part = ""
        if i >= 0:
            j = out.find("=== Method", i + 10)
            part = out[i:j if j > 0 else len(out)]
        notes.append(f"{name}:{'206' if ' 206 ' in part else ('data' if 'abcd' in part else '-')}")
    return "see log; " + ", ".join(notes)


TESTS = [
    ("T01", "board answers", t_alive),
    ("T02", "flash self-test", t_selftest),
    ("T03", "first boot (bootloader, key, watchdog off)", t_first_boot),
    ("T04", "good signed update v1.1", t_good_update),
    ("T05", "crash -> rollback", t_crash_rollback),
    ("T06", "hang -> watchdog -> rollback", t_hang_rollback),
    ("T07", "damaged file refused", t_reject_corrupt),
    ("T08", "old development-key file refused", t_reject_devkey),
    ("T09", "unsigned file refused", t_reject_unsigned),
    ("T10", "power cut during install (you pull battery)", t_power_cut),
    ("T11", "cable reflash detected (optional, ST-LINK)", t_cable_reflash),
    ("T12", "local update server (on this Mac)", t_server),
    ("T13", "N706B modem probe", t_modem),
]


def sign_files(kit: str, key: str, out: str) -> dict:
    bins = os.path.join(kit, "app_bins")
    pub = os.path.join(HERE, "production_public_key.pem")
    k = t.load_private_key(key)
    files = {}
    for tag, name, ver in (("v1.1", "testapp_v1.1", "1.1.0"), ("v1.2", "testapp_v1.2_noconfirm", "1.2.0"),
                           ("v1.3", "testapp_v1.3", "1.3.0"), ("v1.4", "testapp_v1.4_hang", "1.4.0")):
        img = t.build_image(t.read_bytes(os.path.join(bins, name + ".bin")), t.parse_version(ver),
                            t.OTA_HW_ID_GSM_PCB_R1, key=k)
        if tag == "v1.3":
            img = bytearray(img)
            img[len(img) // 2] ^= 0xFF          # damage it after signing
            img = bytes(img)
        else:
            t.check_image(img, pubkey=pub, require_signature=True)
        p = os.path.join(out, name + ".ota")
        with open(p, "wb") as f:
            f.write(img)
        files[tag] = p
    files["v1.5"] = os.path.join(kit, "reject", "testapp_v1.5_devkey.ota")
    files["v1.6"] = os.path.join(kit, "reject", "testapp_v1.6_unsigned.ota")
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--key", required=True, help="your production signing key .pem")
    ap.add_argument("--only", help="comma list of test IDs to run, e.g. T02,T04")
    args = ap.parse_args()

    kit = os.path.dirname(HERE)
    key = os.path.expanduser(args.key)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    results_path = os.path.join(kit, f"ota_test_results_{stamp}.txt")
    log_path = os.path.join(kit, f"ota_test_log_{stamp}.txt")
    work = tempfile.mkdtemp(prefix="ota_kit_")

    print("Signing test apps with your key ...")
    try:
        files = sign_files(kit, key, work)
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: {e}")
        print("Is this the right key? It must match fingerprint", PROD_KEY_FP)
        return 1
    ctx = {"f": files, "key": key,
           "bins": {"v1.1": os.path.join(kit, "app_bins", "testapp_v1.1.bin")}}
    print("  signature OK against the production public key\n")

    only = set(args.only.upper().split(",")) if args.only else None
    rows = []
    with open(log_path, "w") as log:
        b = Board(args.port, log)
        for tid, name, fn in TESTS:
            if only and tid not in only:
                continue
            print(f"[{tid}] {name} ...", flush=True)
            log.write(f"\n\n========== {tid} {name} ==========\n")
            t0 = time.time()
            try:
                note = fn(b, ctx)
                res = "SKIP" if note.startswith("SKIP") else "PASS"
            except Fail as e:
                res, note = "FAIL", str(e)
            except KeyboardInterrupt:
                rows.append((tid, name, "STOP", "stopped by user", 0))
                break
            dt = time.time() - t0
            rows.append((tid, name, res, note, dt))
            print(f"[{tid}] {res}  {note}  ({dt:.0f} s)\n", flush=True)
            log.write(f"\n========== {tid} {res}: {note} ==========\n")
            if res == "FAIL" and tid in ("T01", "T03"):
                print("Basic test failed; stopping here.")
                break
            if res == "FAIL":
                try:
                    b.drain()
                    b.send("h")
                    b.expect("Commands:", timeout=60)
                except Fail:
                    print("Board not answering after a failure; stopping.")
                    break

    with open(results_path, "w") as f:
        f.write(f"Stepwhere OTA bench test {stamp}\n\n")
        for tid, name, res, note, dt in rows:
            f.write(f"{tid} {name:.<52} {res:4}  {note}\n")
        f.write(f"\nfull log: {os.path.basename(log_path)}\n")
    shutil.rmtree(work, ignore_errors=True)

    print("=" * 72)
    with open(results_path) as f:
        print(f.read())
    print(f"Send these two files: {results_path}\n                      {log_path}")
    return 0 if all(r[2] != "FAIL" for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
