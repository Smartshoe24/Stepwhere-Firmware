#!/usr/bin/env python3
"""
ota_tool.py - Package and sign Stepwhere firmware images for OTA.

The binary layout must match libraries/StepwhereOTA/src/ota_layout.h.

Commands
  keygen  Create an ECDSA P-256 signing key pair (run ONCE, keep private key safe)
  pack    Wrap an application .bin into a .ota image (+ manifest.json)
  verify  Check a .ota image (header, CRC, SHA-256, optional signature)
  info    Print the header of a .ota image

Examples
  python tools/ota_tool.py keygen --out keys/
  python tools/ota_tool.py pack --bin build/stepwhere.ino.bin --version 1.2.0 \\
         --key keys/ota_signing_key.pem --out dist/stepwhere_1.2.0.ota \\
         --manifest dist/manifest.json --url https://example.com/fw/stepwhere_1.2.0.ota
  python tools/ota_tool.py verify dist/stepwhere_1.2.0.ota --pubkey keys/ota_public_key.pem

Requires: pip install cryptography   (only for keygen, signing and signature checks)
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass

# ---- must match ota_layout.h -------------------------------------------------
OTA_HDR_MAGIC = 0x57465753          # "SWFW"
OTA_HDR_VERSION = 1
OTA_HDR_SIZE = 256
OTA_HDR_SIGNED_LEN = 128
OTA_FLAG_SIGNED = 1 << 0
OTA_HW_ID_GSM_PCB_R1 = 0x00010001

APP_ADDR = 0x08004000
APP_MAX_SIZE = 112 * 1024
EXT_SLOT_SIZE = 128 * 1024

# struct layout of ota_image_header_t
_HDR_FMT = "<IHHIIIIII32s20s44s64s60sI"
assert struct.calcsize(_HDR_FMT) == OTA_HDR_SIZE


class OtaError(Exception):
    pass


@dataclass
class Header:
    fw_version: int
    image_size: int
    load_addr: int
    hw_id: int
    flags: int
    build_time: int
    payload_sha256: bytes
    git_sha1: bytes
    signature: bytes = b"\x00" * 64
    magic: int = OTA_HDR_MAGIC
    hdr_version: int = OTA_HDR_VERSION
    hdr_size: int = OTA_HDR_SIZE
    hdr_crc32: int = 0

    def pack(self) -> bytes:
        body = struct.pack(
            _HDR_FMT,
            self.magic, self.hdr_version, self.hdr_size, self.fw_version,
            self.image_size, self.load_addr, self.hw_id, self.flags,
            self.build_time, self.payload_sha256, self.git_sha1, b"\x00" * 44,
            self.signature, b"\x00" * 60, 0,
        )
        crc = binascii.crc32(body[:252]) & 0xFFFFFFFF
        return body[:252] + struct.pack("<I", crc)

    @classmethod
    def unpack(cls, raw: bytes) -> "Header":
        if len(raw) < OTA_HDR_SIZE:
            raise OtaError("file is shorter than the 256-byte header")
        f = struct.unpack(_HDR_FMT, raw[:OTA_HDR_SIZE])
        return cls(
            magic=f[0], hdr_version=f[1], hdr_size=f[2], fw_version=f[3],
            image_size=f[4], load_addr=f[5], hw_id=f[6], flags=f[7],
            build_time=f[8], payload_sha256=f[9], git_sha1=f[10],
            signature=f[12], hdr_crc32=f[14],
        )


# ---- helpers -----------------------------------------------------------------

def read_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def parse_version(text: str) -> int:
    """'v1.2', '1.2.3' -> packed 0x00MMmmpp."""
    m = re.fullmatch(r"v?(\d+)\.(\d+)(?:\.(\d+))?", text.strip())
    if not m:
        raise OtaError(f"version must look like 1.2 or 1.2.3, got {text!r}")
    parts = [int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)]
    if any(p > 255 for p in parts):
        raise OtaError("each version part must be 0..255")
    return (parts[0] << 16) | (parts[1] << 8) | parts[2]


def version_str(v: int) -> str:
    return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"


def git_sha1(explicit: str | None) -> bytes:
    sha = explicit or os.environ.get("GITHUB_SHA")
    if not sha:
        try:
            sha = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], stderr=subprocess.DEVNULL, text=True
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            sha = ""
    if not sha:
        return b"\x00" * 20
    if not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise OtaError(f"git sha must be 40 hex characters, got {sha!r}")
    return bytes.fromhex(sha)


def check_app_binary(payload: bytes) -> None:
    """Catch the most common mistake: a .bin linked for 0x08000000."""
    if len(payload) < 8:
        raise OtaError("binary is too small to be an application")
    if len(payload) > APP_MAX_SIZE:
        raise OtaError(f"binary is {len(payload)} bytes, app area is {APP_MAX_SIZE}")
    sp, reset = struct.unpack_from("<II", payload, 0)
    if not (0x20000000 <= sp <= 0x20009000):
        raise OtaError(f"initial stack pointer 0x{sp:08X} is not in RAM; is this a raw .bin?")
    if not (APP_ADDR <= (reset & ~1) < APP_ADDR + APP_MAX_SIZE):
        raise OtaError(
            f"reset handler 0x{reset:08X} is outside the app area "
            f"0x{APP_ADDR:08X}..0x{APP_ADDR + APP_MAX_SIZE:08X}. "
            "The firmware must be linked to start at 0x08004000 (bootloader in front)."
        )


def _crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        raise OtaError("this command needs the 'cryptography' package: pip install cryptography")
    return hashes, serialization, ec, utils


def load_private_key(path: str | None):
    hashes, serialization, ec, utils = _crypto()
    pem = os.environ.get("OTA_SIGNING_KEY") if path is None else read_bytes(path)
    if pem is None:
        raise OtaError("no key file given and OTA_SIGNING_KEY is not set")
    if isinstance(pem, str):
        pem = pem.encode()
    key = serialization.load_pem_private_key(pem, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise OtaError("signing key must be an ECDSA P-256 (secp256r1) private key")
    return key


def sign_header(hdr_bytes: bytes, key) -> bytes:
    hashes, _, ec, utils = _crypto()
    der = key.sign(hdr_bytes[:OTA_HDR_SIGNED_LEN], ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_signature(hdr_bytes: bytes, sig: bytes, pubkey_path: str) -> bool:
    hashes, serialization, ec, utils = _crypto()
    from cryptography.exceptions import InvalidSignature
    pub = serialization.load_pem_public_key(read_bytes(pubkey_path))
    der = utils.encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        pub.verify(der, hdr_bytes[:OTA_HDR_SIGNED_LEN], ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


# ---- core operations (also used by tests) ------------------------------------

def build_image(payload: bytes, version: int, hw_id: int, key=None,
                build_time: int | None = None, sha1: bytes = b"\x00" * 20,
                skip_app_check: bool = False) -> bytes:
    if not skip_app_check:
        check_app_binary(payload)
    hdr = Header(
        fw_version=version, image_size=len(payload), load_addr=APP_ADDR,
        hw_id=hw_id, flags=OTA_FLAG_SIGNED if key else 0,
        build_time=int(time.time()) if build_time is None else build_time,
        payload_sha256=hashlib.sha256(payload).digest(), git_sha1=sha1,
    )
    raw = hdr.pack()
    if key is not None:
        hdr.signature = sign_header(raw, key)
        raw = hdr.pack()
    image = raw + payload
    if len(image) > EXT_SLOT_SIZE:
        raise OtaError(f"image is {len(image)} bytes, slot is {EXT_SLOT_SIZE}")
    return image


def check_image(image: bytes, pubkey: str | None = None, require_signature: bool = False) -> Header:
    hdr = Header.unpack(image)
    if hdr.magic != OTA_HDR_MAGIC:
        raise OtaError(f"bad magic 0x{hdr.magic:08X}")
    if hdr.hdr_version != OTA_HDR_VERSION or hdr.hdr_size != OTA_HDR_SIZE:
        raise OtaError(f"unsupported header version {hdr.hdr_version} / size {hdr.hdr_size}")
    crc = binascii.crc32(image[:252]) & 0xFFFFFFFF
    if crc != hdr.hdr_crc32:
        raise OtaError(f"header CRC mismatch (stored 0x{hdr.hdr_crc32:08X}, computed 0x{crc:08X})")
    payload = image[OTA_HDR_SIZE:]
    if len(payload) != hdr.image_size:
        raise OtaError(f"payload is {len(payload)} bytes, header says {hdr.image_size}")
    if hashlib.sha256(payload).digest() != hdr.payload_sha256:
        raise OtaError("payload SHA-256 mismatch")
    if hdr.load_addr != APP_ADDR:
        raise OtaError(f"load address 0x{hdr.load_addr:08X} != 0x{APP_ADDR:08X}")
    signed = bool(hdr.flags & OTA_FLAG_SIGNED)
    if require_signature and not signed:
        raise OtaError("image is not signed")
    if pubkey:
        if not signed:
            raise OtaError("image is not signed")
        if not verify_signature(image, hdr.signature, pubkey):
            raise OtaError("signature is INVALID")
    return hdr


def manifest_for(image: bytes, hdr: Header, url: str | None) -> dict:
    return {
        "version": version_str(hdr.fw_version),
        "version_code": hdr.fw_version,
        "hw_id": f"0x{hdr.hw_id:08X}",
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "signed": bool(hdr.flags & OTA_FLAG_SIGNED),
        "git_sha": hdr.git_sha1.hex(),
        "build_time": hdr.build_time,
        "url": url or "",
    }


# ---- CLI ---------------------------------------------------------------------

def cmd_keygen(args) -> None:
    _, serialization, ec, _ = _crypto()
    os.makedirs(args.out, exist_ok=True)
    priv_path = os.path.join(args.out, "ota_signing_key.pem")
    pub_path = os.path.join(args.out, "ota_public_key.pem")
    hdr_path = os.path.join(args.out, "ota_public_key.h")
    if os.path.exists(priv_path) and not args.force:
        raise OtaError(f"{priv_path} already exists; use --force to overwrite (old devices would stop accepting updates)")
    key = ec.generate_private_key(ec.SECP256R1())
    with open(priv_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    os.chmod(priv_path, 0o600)
    pub = key.public_key()
    with open(pub_path, "wb") as f:
        f.write(pub.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    raw = pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)[1:]
    lines = ", ".join(f"0x{b:02X}" for b in raw)
    with open(hdr_path, "w") as f:
        f.write("/* Generated by tools/ota_tool.py keygen. Public key: safe to commit. */\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("/* ECDSA P-256 public key, raw X||Y (64 bytes) */\n")
        f.write(f"static const uint8_t OTA_PUBLIC_KEY[64] = {{ {lines} }};\n")
    print(f"private key : {priv_path}  <- keep secret, store as GitHub secret OTA_SIGNING_KEY, back it up")
    print(f"public key  : {pub_path}")
    print(f"C header    : {hdr_path}  <- goes into the bootloader")


def cmd_pack(args) -> None:
    payload = read_bytes(args.bin)
    key = load_private_key(args.key) if (args.key or args.sign_from_env) else None
    image = build_image(payload, parse_version(args.version), int(args.hw_id, 0),
                        key=key, sha1=git_sha1(args.git_sha))
    hdr = check_image(image)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)
    print(f"wrote {args.out}: v{version_str(hdr.fw_version)}, payload {hdr.image_size} bytes, "
          f"{'signed' if key else 'UNSIGNED'}")
    if args.manifest:
        with open(args.manifest, "w") as f:
            json.dump(manifest_for(image, hdr, args.url), f, indent=2)
            f.write("\n")
        print(f"wrote {args.manifest}")


def cmd_verify(args) -> None:
    image = read_bytes(args.image)
    hdr = check_image(image, pubkey=args.pubkey, require_signature=args.require_signature)
    sig = "signature OK" if args.pubkey else ("signed (not checked)" if hdr.flags & OTA_FLAG_SIGNED else "unsigned")
    print(f"OK: v{version_str(hdr.fw_version)}, {hdr.image_size} bytes, {sig}")


def cmd_info(args) -> None:
    hdr = Header.unpack(read_bytes(args.image))
    print(f"magic       0x{hdr.magic:08X}")
    print(f"hdr_version {hdr.hdr_version}")
    print(f"fw_version  {version_str(hdr.fw_version)} (0x{hdr.fw_version:08X})")
    print(f"image_size  {hdr.image_size}")
    print(f"load_addr   0x{hdr.load_addr:08X}")
    print(f"hw_id       0x{hdr.hw_id:08X}")
    print(f"flags       0x{hdr.flags:08X}")
    print(f"build_time  {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(hdr.build_time))}")
    print(f"sha256      {hdr.payload_sha256.hex()}")
    print(f"git_sha1    {hdr.git_sha1.hex()}")
    print(f"hdr_crc32   0x{hdr.hdr_crc32:08X}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    k = sub.add_parser("keygen", help="create a signing key pair")
    k.add_argument("--out", default="keys")
    k.add_argument("--force", action="store_true")
    k.set_defaults(func=cmd_keygen)

    pk = sub.add_parser("pack", help="build a .ota image from an app .bin")
    pk.add_argument("--bin", required=True, help="application binary linked at 0x08004000")
    pk.add_argument("--version", required=True, help="e.g. 1.2.0 or v1.2")
    pk.add_argument("--hw-id", default=hex(OTA_HW_ID_GSM_PCB_R1))
    pk.add_argument("--key", help="PEM private key file")
    pk.add_argument("--sign-from-env", action="store_true", help="read PEM key from $OTA_SIGNING_KEY")
    pk.add_argument("--git-sha", help="commit SHA (default: $GITHUB_SHA or git rev-parse HEAD)")
    pk.add_argument("--out", required=True)
    pk.add_argument("--manifest", help="also write manifest.json here")
    pk.add_argument("--url", help="download URL to put in the manifest")
    pk.set_defaults(func=cmd_pack)

    v = sub.add_parser("verify", help="check a .ota image")
    v.add_argument("image")
    v.add_argument("--pubkey", help="PEM public key; checks the signature")
    v.add_argument("--require-signature", action="store_true")
    v.set_defaults(func=cmd_verify)

    i = sub.add_parser("info", help="print a .ota header")
    i.add_argument("image")
    i.set_defaults(func=cmd_info)

    args = p.parse_args(argv)
    try:
        args.func(args)
    except (OtaError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
