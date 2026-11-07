#!/usr/bin/env python3
"""
fw_server.py - Local firmware update server (stand-in for S3 + CloudFront).

Same layout as the AWS bucket, so devices only need a different base URL
when we move to AWS:

  <root>/manifest.json                    newest release
  <root>/releases/<version>/stepwhere.ota

Commands
  publish  pack + sign an app .bin and publish it (image first, manifest last)
  serve    HTTP server with byte-range support (resumable downloads) and a
           request log, so you can see what the device asks for

Examples (on the Mac that holds the signing key)
  python3 tools/fw_server.py publish --bin prebuilt/ota_test/app_bins/testapp_v1.1.bin \\
      --version 1.1.0 --key ~/stepwhere-keys/ota_signing_key.pem
  python3 tools/fw_server.py serve --port 8080

The N706B reaches the Mac over the mobile network, so the server must be
reachable from the internet: run a tunnel next to it, e.g.
  ssh -p 443 -R0:localhost:8080 a.pinggy.io
and use the http:// address it prints as the device's update URL.

Standard library only (publish also needs "cryptography" for signing).
"""
from __future__ import annotations

import argparse
import http.server
import json
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

DEFAULT_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fw_server_root")


# ---------------------------------------------------------------- publish ---

def cmd_publish(args) -> int:
    import ota_tool as t

    root = os.path.abspath(args.root)
    version = t.version_str(t.parse_version(args.version))
    manifest_path = os.path.join(root, "manifest.json")

    if os.path.exists(manifest_path) and not args.allow_older:
        with open(manifest_path) as f:
            cur = json.load(f)["version_code"]
        if t.parse_version(version) <= cur:
            print(f"error: {version} is not newer than the published "
                  f"{t.version_str(cur)} (use --allow-older for bench tests)", file=sys.stderr)
            return 1

    key = t.load_private_key(args.key)
    image = t.build_image(t.read_bytes(args.bin), t.parse_version(version), int(args.hw_id, 0),
                          key=key, sha1=t.git_sha1(None))
    hdr = t.check_image(image)
    if args.pubkey:   # refuse to publish anything the devices would reject
        t.check_image(image, pubkey=args.pubkey, require_signature=True)

    rel = f"releases/{version}/stepwhere.ota"
    img_path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(img_path), exist_ok=True)
    with open(img_path + ".tmp", "wb") as f:
        f.write(image)
    os.replace(img_path + ".tmp", img_path)

    # Manifest last, atomically: a device never sees a manifest for a missing image.
    with open(manifest_path + ".tmp", "w") as f:
        json.dump(t.manifest_for(image, hdr, rel), f, indent=2)
        f.write("\n")
    os.replace(manifest_path + ".tmp", manifest_path)
    print(f"published {version}: {rel} ({len(image)} bytes) + manifest.json in {root}")
    return 0


# ------------------------------------------------------------------ serve ---

class Handler(http.server.SimpleHTTPRequestHandler):
    server_version = "stepwhere-fw/1"

    def log_message(self, fmt, *a):
        sys.stderr.write(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} {fmt % a}\n")

    def end_headers(self):
        path = self.path.split("?")[0]
        if path.endswith("manifest.json"):
            self.send_header("Cache-Control", "public, max-age=60")
        elif path.endswith(".ota"):
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404)
        return None

    def send_head(self):
        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.isfile(path):
            self.send_error(404)
            return None
        rng = self.headers.get("Range")
        f = open(path, "rb")
        size = os.fstat(f.fileno()).st_size
        ctype = "application/json" if path.endswith(".json") else "application/octet-stream"
        if not rng:
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(size))
            self.end_headers()
            return f
        m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
        if not m or (m.group(1) == "" and m.group(2) == ""):
            f.close()
            self.send_error(416)
            return None
        if m.group(1) == "":                       # suffix range: last N bytes
            start, end = max(0, size - int(m.group(2))), size - 1
        else:
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        if start >= size or start > end:
            f.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        self.send_response(206)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        f.seek(start)
        self._range_left = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        left = getattr(self, "_range_left", None)
        if left is None:
            shutil.copyfileobj(source, outputfile)
            return
        while left > 0:
            buf = source.read(min(65536, left))
            if not buf:
                break
            outputfile.write(buf)
            left -= len(buf)


def cmd_serve(args) -> int:
    root = os.path.abspath(args.root)
    os.makedirs(root, exist_ok=True)
    handler = lambda *a, **kw: Handler(*a, directory=root, **kw)  # noqa: E731
    srv = http.server.ThreadingHTTPServer((args.bind, args.port), handler)
    print(f"serving {root} on http://{args.bind}:{args.port}/  (Ctrl-C to stop)")
    mp = os.path.join(root, "manifest.json")
    if os.path.exists(mp):
        with open(mp) as f:
            print("current release:", json.load(f)["version"])
    else:
        print("no release published yet (run: fw_server.py publish ...)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--root", default=DEFAULT_ROOT, help="server folder (default: fw_server_root/)")
    sub = p.add_subparsers(dest="cmd", required=True)

    pb = sub.add_parser("publish", help="sign and publish an app .bin")
    pb.add_argument("--bin", required=True, help="app .bin linked at 0x08004000")
    pb.add_argument("--version", required=True)
    pb.add_argument("--key", required=True, help="signing key .pem")
    pb.add_argument("--pubkey", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                     "production_public_key.pem"),
                    help="check the signature against this public key before publishing")
    pb.add_argument("--hw-id", default="0x00010001")
    pb.add_argument("--allow-older", action="store_true", help="allow re-publishing an older version")
    pb.set_defaults(func=cmd_publish)

    sv = sub.add_parser("serve", help="run the HTTP server")
    sv.add_argument("--port", type=int, default=8080)
    sv.add_argument("--bind", default="0.0.0.0")
    sv.set_defaults(func=cmd_serve)

    args = p.parse_args(argv)
    try:
        return args.func(args)
    except Exception as e:  # noqa: BLE001
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
