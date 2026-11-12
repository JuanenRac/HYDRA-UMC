# =============================================================================
# HYDRA-UMC - tests/test_image_header_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The C vectors (tests/image_header_vectors.h) are what the host reference makes, and the reference accepts and refuses them as the
bootloader's C code does. The C side of the same bytes is tests/test_image_header.c, tests/test_update_verify.c and
tests/test_bootloader_g474_host.c (build_firmware.sh compiles them with the host C compiler)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_image_header_vectors as gen  # noqa: E402
import image_header as ih  # noqa: E402

BOOT_DIRECTORIES = ("src/mcu_stm32g474/boot", "src/mcu_stm32h757/CM4/boot", "src/mcu_stm32h757/CM7/boot")
SHARED_HEADERS = ("image_header.h", "update_verify.h", "update_signature.h", "update_signature.c", "tweetnacl.c", "tweetnacl.h", "bootloader_trust.h", "bootloader_trust.c",
                  "hydra_trust_dev.h", "hydra_trust_release.h")


def _blob(version, seed):
    header, image, signature, digest = gen.signed(version, seed)
    return header.pack() + image + signature, digest


def _verify(blob, installed, **over):
    args = dict(hw_id=gen.HW_ID, link_address=gen.LINK, installed_version=installed, ram_range=(gen.RAM_LO, gen.RAM_HI))
    args.update(over)
    return ih.verify_image(blob, gen.PUBLIC, **args)


class VectorTests(unittest.TestCase):
    def test_the_header_file_is_what_the_reference_makes(self):
        current = gen.OUTPUT.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(current, gen.render(), "run python tools/gen_image_header_vectors.py")

    def test_image_a_is_an_update_over_the_installed_version(self):
        blob, _ = _blob(gen.VERSION_A, 7)
        self.assertEqual(_verify(blob, gen.INSTALLED).version, gen.VERSION_A)
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            _verify(blob, gen.VERSION_A)

    def test_image_b_is_older_and_needs_the_token(self):
        blob, digest = _blob(gen.VERSION_B, 11)
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            _verify(blob, gen.INSTALLED)
        token = ih.downgrade_token(gen.SEED, gen.NONCE, gen.INSTALLED, gen.VERSION_B, digest)
        self.assertEqual(_verify(blob, gen.INSTALLED, nonce=gen.NONCE, token=token).version, gen.VERSION_B)
        # the token is for this image: the digest of image A does not make one that opens B
        other = ih.downgrade_token(gen.SEED, gen.NONCE, gen.INSTALLED, gen.VERSION_B, _blob(gen.VERSION_A, 7)[1])
        with self.assertRaises(ih.ImageError):
            _verify(blob, gen.INSTALLED, nonce=gen.NONCE, token=other)

    def test_the_three_bootloaders_hold_the_same_shared_headers(self):
        for name in SHARED_HEADERS:
            reference = (ROOT / BOOT_DIRECTORIES[0] / name).read_bytes().replace(b"\r\n", b"\n")
            for directory in BOOT_DIRECTORIES[1:]:
                self.assertEqual((ROOT / directory / name).read_bytes().replace(b"\r\n", b"\n"), reference, f"{directory}/{name} differs from {BOOT_DIRECTORIES[0]}/{name}")


class DevelopmentKeyTests(unittest.TestCase):
    def test_the_development_table_of_the_bootloaders_holds_the_public_key_of_the_published_seed(self):
        import ed25519_ref
        import image_header2 as h2
        import image_trust as trust

        doc = trust.load()
        dev = doc["flavours"]["dev"]["keys"]
        self.assertEqual(len(dev), 1)
        self.assertEqual(bytes.fromhex(dev[0]["public"]), ed25519_ref.public_key(h2.DEV_SEED))
        for directory in BOOT_DIRECTORIES:
            text = (ROOT / directory / "hydra_trust_dev.h").read_bytes().decode("utf-8")
            self.assertEqual(text.splitlines(), trust.header_text(doc, "dev").splitlines(), directory)

    def test_the_bootloaders_hold_no_key_and_no_hmac(self):
        for directory in BOOT_DIRECTORIES:
            for name in ("bootloader_crypto.c", "bootloader_crypto.h", "bootloader_common.h"):
                text = (ROOT / directory / name).read_text(encoding="utf-8")
                self.assertNotIn("HMAC_KEY", text, f"{directory}/{name}")
                self.assertNotIn("UPDATE_PUBLIC_KEY", text, f"{directory}/{name}")
                self.assertNotIn("hmac_sha256", text, f"{directory}/{name}")


if __name__ == "__main__":
    unittest.main()
