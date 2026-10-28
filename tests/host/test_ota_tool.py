#!/usr/bin/env python3
"""Tests for tools/ota_tool.py. Run: python3 tests/host/test_ota_tool.py"""
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import ota_tool as t  # noqa: E402


def fake_app(size=73 * 1024, sp=0x20009000, reset=0x08004101):
    body = bytes((i * 13 + 5) & 0xFF for i in range(size - 8))
    return struct.pack("<II", sp, reset) + body


class VersionTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(t.parse_version("v1.2"), 0x010200)
        self.assertEqual(t.parse_version("1.2.3"), 0x010203)
        self.assertEqual(t.version_str(0x010203), "1.2.3")
        for bad in ("1", "1.2.3.4", "abc", "1.256"):
            with self.assertRaises(t.OtaError):
                t.parse_version(bad)


class AppCheckTests(unittest.TestCase):
    def test_accepts_app_linked_at_0x08004000(self):
        t.check_app_binary(fake_app())

    def test_rejects_app_linked_at_0x08000000(self):
        with self.assertRaisesRegex(t.OtaError, "0x08004000"):
            t.check_app_binary(fake_app(reset=0x080001C1))

    def test_rejects_non_binary(self):
        with self.assertRaisesRegex(t.OtaError, "stack pointer"):
            t.check_app_binary(fake_app(sp=0x3A3A3A3A))

    def test_rejects_too_big(self):
        with self.assertRaisesRegex(t.OtaError, "app area"):
            t.check_app_binary(fake_app(size=t.APP_MAX_SIZE + 4))


class ImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t.main(["keygen", "--out", cls.tmp.name])
        cls.priv = os.path.join(cls.tmp.name, "ota_signing_key.pem")
        cls.pub = os.path.join(cls.tmp.name, "ota_public_key.pem")
        cls.key = t.load_private_key(cls.priv)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def image(self, signed=True):
        return t.build_image(fake_app(), t.parse_version("1.3.0"), t.OTA_HW_ID_GSM_PCB_R1,
                             key=self.key if signed else None, build_time=1_800_000_000)

    def test_header_fields(self):
        img = self.image()
        payload = img[t.OTA_HDR_SIZE:]
        hdr = t.check_image(img, pubkey=self.pub, require_signature=True)
        self.assertEqual(img[:4], b"SWFW")
        self.assertEqual(hdr.fw_version, 0x010300)
        self.assertEqual(hdr.image_size, len(payload))
        self.assertEqual(hdr.load_addr, 0x08004000)
        self.assertEqual(hdr.payload_sha256, hashlib.sha256(payload).digest())
        self.assertEqual(len(img), 256 + 73 * 1024)

    def test_unsigned_image(self):
        img = self.image(signed=False)
        t.check_image(img)
        with self.assertRaises(t.OtaError):
            t.check_image(img, require_signature=True)
        with self.assertRaises(t.OtaError):
            t.check_image(img, pubkey=self.pub)

    def test_payload_tamper_detected(self):
        img = bytearray(self.image())
        img[5000] ^= 0x01
        with self.assertRaisesRegex(t.OtaError, "SHA-256"):
            t.check_image(bytes(img))

    def test_header_tamper_detected_by_crc(self):
        img = bytearray(self.image())
        img[8] ^= 0x01                      # fw_version
        with self.assertRaisesRegex(t.OtaError, "CRC"):
            t.check_image(bytes(img))

    def test_header_tamper_with_fixed_crc_fails_signature(self):
        hdr = t.Header.unpack(self.image())
        hdr.fw_version = t.parse_version("9.9.9")   # attacker bumps version, fixes CRC
        img = hdr.pack() + self.image()[t.OTA_HDR_SIZE:]
        t.check_image(img)                          # integrity checks still pass ...
        with self.assertRaisesRegex(t.OtaError, "INVALID"):
            t.check_image(img, pubkey=self.pub)     # ... but the signature does not

    def test_wrong_key_rejected(self):
        with tempfile.TemporaryDirectory() as other:
            t.main(["keygen", "--out", other])
            with self.assertRaisesRegex(t.OtaError, "INVALID"):
                t.check_image(self.image(), pubkey=os.path.join(other, "ota_public_key.pem"))

    def test_truncated_image(self):
        with self.assertRaisesRegex(t.OtaError, "payload is"):
            t.check_image(self.image()[:-10])

    def test_cli_pack_verify_and_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            binp = os.path.join(d, "app.bin")
            with open(binp, "wb") as f:
                f.write(fake_app())
            out = os.path.join(d, "fw.ota")
            man = os.path.join(d, "manifest.json")
            rc = t.main(["pack", "--bin", binp, "--version", "v1.2", "--key", self.priv,
                         "--git-sha", "a" * 40, "--out", out, "--manifest", man,
                         "--url", "https://example.com/fw.ota"])
            self.assertEqual(rc, 0)
            self.assertEqual(t.main(["verify", out, "--pubkey", self.pub]), 0)
            m = json.loads(t.read_bytes(man))
            self.assertEqual(m["version"], "1.2.0")
            self.assertEqual(m["size"], os.path.getsize(out))
            self.assertEqual(m["sha256"], hashlib.sha256(t.read_bytes(out)).hexdigest())
            self.assertTrue(m["signed"])
            self.assertEqual(m["git_sha"], "a" * 40)

    def test_cli_rejects_wrongly_linked_bin(self):
        with tempfile.TemporaryDirectory() as d:
            binp = os.path.join(d, "app.bin")
            with open(binp, "wb") as f:
                f.write(fake_app(reset=0x080001C1))
            self.assertEqual(t.main(["pack", "--bin", binp, "--version", "1.0",
                                     "--out", os.path.join(d, "x.ota")]), 1)

    def test_c_header_has_64_byte_key(self):
        text = t.read_bytes(os.path.join(self.tmp.name, "ota_public_key.h")).decode()
        self.assertEqual(text.count("0x"), 64)


if __name__ == "__main__":
    unittest.main(verbosity=1)
