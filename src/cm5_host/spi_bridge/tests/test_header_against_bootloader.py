# =============================================================================
# HYDRA-UMC - spi_bridge: the header of the CM5 against the parser of the bootloaders
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The header that spi_bridge.image_header builds is given to the C code of the three bootloaders (image_header.h and update_verify.h,
compiled for the host): ImageHeader_Parse2 reads it and UpdateVerify_Decide2 decides. The program records the message the bootloader asks
the signature check about, and this test verifies the Ed25519 signature made with the host reference over exactly that message. Evidence:
simulator (nothing runs on a board)."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from spi_bridge import image_header as client_header

REPOSITORY = Path(__file__).resolve().parents[4]
TOOLS = REPOSITORY / "tools"
BOOTLOADERS = {
    0x47343031: REPOSITORY / "src/mcu_stm32g474/boot",
    0x48374337: REPOSITORY / "src/mcu_stm32h757/CM7/boot",
    0x48374334: REPOSITORY / "src/mcu_stm32h757/CM4/boot",
}
if TOOLS.is_dir():
    sys.path.insert(0, str(TOOLS))
    import ed25519_ref  # noqa: E402
    import image_header2 as reference2  # noqa: E402

COMPILER = next((path for path in (shutil.which(name) for name in (os.environ.get("CC") or "cc", "gcc", "clang")) if path), None)
# On a build service a missing compiler is a failure, never a silent skip.
REQUIRED = os.environ.get("GITHUB_ACTIONS") == "true"

# UpdateVerdict_t
OK, SIGNATURE, HEADER, UNKNOWN_KEY, COUNTER = 0, 3, 4, 7, 8

PROGRAM = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update_verify.h"

static void unhex(const char *text, uint8_t *out, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        unsigned value = 0;
        if (sscanf(text + 2 * i, "%2x", &value) != 1) exit(2);
        out[i] = (uint8_t)value;
    }
}

static int record(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], const uint8_t public_key[32], void *context)
{
    (void)signature; (void)public_key; (void)context;
    if (len != IMAGE_MESSAGE_2_LEN) return 0;
    printf("message ");
    for (uint32_t i = 0; i < len; i++) printf("%02x", msg[i]);
    printf("\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 15) return 2;
    static uint8_t header[IMAGE_HEADER_SIZE], digest[IMAGE_DIGEST_SIZE], signature[IMAGE_SIGNATURE_SIZE], nonce[IMAGE_NONCE_SIZE];
    static uint8_t ids[1][IMAGE_KEY_ID_SIZE], keys[1][32];
    unhex(argv[1], header, sizeof header);
    unhex(argv[2], digest, sizeof digest);
    unhex(argv[3], ids[0], sizeof ids[0]);

    UpdateVerify2Input_t in;
    memset(&in, 0, sizeof in);
    in.total_size = in.bytes_received = (uint32_t)strtoul(argv[4], NULL, 0);
    in.declared_crc = in.actual_crc = (uint32_t)strtoul(argv[5], NULL, 0);
    in.expect.hw_id = (uint32_t)strtoul(argv[6], NULL, 0);
    in.expect.link_address = (uint32_t)strtoul(argv[7], NULL, 0);
    in.expect.first_word = (uint32_t)strtoul(argv[8], NULL, 0);
    in.expect.second_word = (uint32_t)strtoul(argv[9], NULL, 0);
    in.expect.ram_lo = 0x20000000u;
    in.expect.ram_hi = 0x20030000u;
    in.counter_floor = (uint32_t)strtoul(argv[10], NULL, 0);
    in.declared_major = (uint16_t)strtoul(argv[11], NULL, 0);
    in.declared_minor = (uint16_t)strtoul(argv[12], NULL, 0);
    in.have_installed = 1;
    in.installed.major = (uint16_t)strtoul(argv[13], NULL, 0);
    in.installed.minor = 0;
    in.installed.patch = (uint16_t)strtoul(argv[14], NULL, 0);
    in.signature_chunks = UPDATE_SIGNATURE_CHUNKS;
    in.header_chunks = UPDATE_HEADER_CHUNKS;
    in.header = header;
    in.signature = signature;
    in.actual_digest = digest;
    in.token = signature;
    in.nonce = nonce;
    in.key_count = 1;
    in.key_ids = (const uint8_t (*)[IMAGE_KEY_ID_SIZE])ids;
    in.keys = (const uint8_t (*)[32])keys;
    in.verify = record;

    ImageHeader2_t parsed;
    memset(&parsed, 0, sizeof parsed);
    UpdateVerdict_t verdict = UpdateVerify_Decide2(&in, &parsed);
    printf("verdict %d\n", (int)verdict);
    if (verdict == UPDATE_VERDICT_OK) {
        printf("version %u.%u.%u counter %lu\n", (unsigned)parsed.base.version.major, (unsigned)parsed.base.version.minor,
               (unsigned)parsed.base.version.patch, (unsigned long)parsed.rollback_counter);
    }
    return 0;
}
"""

SEED = bytes(range(0x50, 0x70))  # a throwaway key of the tests


@unittest.skipIf(not TOOLS.is_dir(), "the package deployed on its own: no bootloader sources next to it")
@unittest.skipIf(COMPILER is None and not REQUIRED, "no host C compiler")
class HeaderAgainstTheBootloadersTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if COMPILER is None:
            raise AssertionError("a host C compiler is needed to check the header against the bootloaders")
        cls.folder = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        source = Path(cls.folder.name) / "check_header.c"
        source.write_text(PROGRAM, encoding="utf-8")
        cls.programs = {}
        for hardware_id, include in BOOTLOADERS.items():
            program = Path(cls.folder.name) / f"check_{hardware_id:08x}.exe"
            built = subprocess.run(
                [COMPILER, "-std=c11", "-Wall", "-Werror", f"-I{include}", str(source), "-o", str(program)],
                capture_output=True, text=True, check=False, timeout=120,
            )
            if built.returncode != 0:
                raise AssertionError(built.stderr)
            cls.programs[hardware_id] = program

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def decide(self, hardware_id, header, image, *, version=(0, 3, 0), floor=0, key_id=None):
        link = client_header.LINK_ADDRESS_BY_HARDWARE_ID[hardware_id]
        stack, reset = struct.unpack_from("<II", image, 0)
        public = ed25519_ref.public_key(SEED)
        arguments = [
            header.hex(), hashlib.sha256(header + image).hexdigest(), (key_id or reference2.key_id(public)).hex(),
            len(image), zlib.crc32(image), hardware_id, link, stack, reset, floor, version[0], version[1], 0, 0,
        ]
        run = subprocess.run([str(self.programs[hardware_id]), *[str(a) for a in arguments]], capture_output=True, text=True, check=True, timeout=60)
        lines = dict(line.split(" ", 1) for line in run.stdout.splitlines())
        return int(lines["verdict"]), lines

    def image_and_header(self, hardware_id, counter=3):
        link = client_header.LINK_ADDRESS_BY_HARDWARE_ID[hardware_id]
        image = struct.pack("<II", 0x20010000, link + 0x101) + bytes((i * 7 + 3) & 0xFF for i in range(1016))
        key_id = reference2.key_id(ed25519_ref.public_key(SEED))
        return image, client_header.build_header(hardware_id, (0, 3, 0), image, counter, key_id)

    def test_every_bootloader_accepts_the_header_and_asks_about_the_signed_message(self):
        for hardware_id in BOOTLOADERS:
            with self.subTest(hardware_id=hex(hardware_id)):
                image, header = self.image_and_header(hardware_id)
                verdict, lines = self.decide(hardware_id, header, image, floor=3)
                self.assertEqual(verdict, OK, lines)
                self.assertEqual(lines["version"], "0.3.0 counter 3")
                message = bytes.fromhex(lines["message"])
                self.assertEqual(message, client_header.image_message(client_header.image_digest(header, image)))
                signature = ed25519_ref.sign(SEED, reference2.image_message2(header, image))
                self.assertTrue(ed25519_ref.verify(ed25519_ref.public_key(SEED), message, signature))

    def test_every_bootloader_refuses_a_header_of_format_1(self):
        for hardware_id in BOOTLOADERS:
            with self.subTest(hardware_id=hex(hardware_id)):
                image, header = self.image_and_header(hardware_id)
                old = header[:4] + struct.pack("<H", 1) + header[6:40] + bytes(24)
                self.assertEqual(self.decide(hardware_id, old, image)[0], HEADER)

    def test_the_counter_and_the_key_id_are_read_where_the_client_puts_them(self):
        for hardware_id in BOOTLOADERS:
            with self.subTest(hardware_id=hex(hardware_id)):
                image, header = self.image_and_header(hardware_id, counter=3)
                self.assertEqual(self.decide(hardware_id, header, image, floor=4)[0], COUNTER)
                self.assertEqual(self.decide(hardware_id, header, image, key_id=bytes(8))[0], UNKNOWN_KEY)


if __name__ == "__main__":
    unittest.main()
