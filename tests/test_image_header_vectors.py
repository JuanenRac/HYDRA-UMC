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

BOOT_DIRECTORIES = ("src/mcu_stm32g474/boot", "src/mcu_stm32h745/CM4/boot", "src/mcu_stm32h745/CM7/boot")
SHARED_HEADERS = ("image_header.h", "update_verify.h", "update_signature.h", "update_signature.c", "tweetnacl.c", "tweetnacl.h")


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
    def test_the_development_public_keys_in_the_bootloaders_are_those_of_the_published_seeds(self):
        import ed25519_ref

        for name, directory in (("G401", BOOT_DIRECTORIES[0]), ("H7C4", BOOT_DIRECTORIES[1]), ("H7C7", BOOT_DIRECTORIES[2])):
            text = (ROOT / directory / "bootloader_crypto.c").read_text(encoding="utf-8")
            start = text.index("const uint8_t UPDATE_PUBLIC_KEY[32] = {\n") + len("const uint8_t UPDATE_PUBLIC_KEY[32] = {\n")
            body = text[start:text.index("};", start)]
            key = bytes(int(token, 16) for token in body.replace("\n", " ").replace(",", " ").split())
            self.assertEqual(key, ed25519_ref.public_key(ih.DEV_SEEDS[name]), f"{directory}: the development key is not the one of the {name} seed")


if __name__ == "__main__":
    unittest.main()
