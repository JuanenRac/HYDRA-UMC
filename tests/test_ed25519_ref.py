# =============================================================================
# HYDRA-UMC - tests/test_ed25519_ref.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/ed25519_ref.py against the vectors of RFC 8032 section 7.1 and, when the `cryptography` package is installed, against OpenSSL."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import ed25519_ref as ed  # noqa: E402

RFC_VECTORS = (
    (
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
)


class Ed25519ReferenceTests(unittest.TestCase):
    def test_the_vectors_of_rfc_8032(self):
        for seed, public, message, signature in RFC_VECTORS:
            seed_b, message_b = bytes.fromhex(seed), bytes.fromhex(message)
            self.assertEqual(ed.public_key(seed_b).hex(), public)
            self.assertEqual(ed.sign(seed_b, message_b).hex(), signature)
            self.assertTrue(ed.verify(bytes.fromhex(public), message_b, bytes.fromhex(signature)))

    def test_a_changed_message_signature_or_key_does_not_verify(self):
        seed = bytes(range(32))
        public = ed.public_key(seed)
        signature = ed.sign(seed, b"hello")
        self.assertTrue(ed.verify(public, b"hello", signature))
        self.assertFalse(ed.verify(public, b"hellO", signature))
        for position in (0, 31, 32, 63):
            broken = bytearray(signature)
            broken[position] ^= 1
            self.assertFalse(ed.verify(public, b"hello", bytes(broken)), position)
        self.assertFalse(ed.verify(ed.public_key(bytes(32)), b"hello", signature))
        self.assertFalse(ed.verify(public, b"hello", signature[:63]))

    def test_a_scalar_that_is_not_reduced_is_refused(self):
        seed = bytes(range(32))
        public = ed.public_key(seed)
        signature = ed.sign(seed, b"m")
        s = int.from_bytes(signature[32:], "little") + (2**252 + 27742317777372353535851937790883648493)
        if s < 2**256:
            self.assertFalse(ed.verify(public, b"m", signature[:32] + s.to_bytes(32, "little")))

    def test_the_seed_must_be_32_bytes(self):
        with self.assertRaises(ValueError):
            ed.public_key(b"short")

    def test_it_agrees_with_openssl_when_it_is_there(self):
        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        except ImportError:
            self.skipTest("the cryptography package is not installed")
        for index in range(5):
            seed = bytes((index * 37 + i * 11) & 0xFF for i in range(32))
            message = bytes((index + i) & 0xFF for i in range(index * 17))
            key = Ed25519PrivateKey.from_private_bytes(seed)
            raw_public = key.public_key().public_bytes_raw()
            self.assertEqual(ed.public_key(seed), raw_public)
            self.assertEqual(ed.sign(seed, message), key.sign(message))


if __name__ == "__main__":
    unittest.main()
