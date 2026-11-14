"""Header format 2, the trust table and its public source file: the host reference held to the shared vectors and to hand-written verdicts.
Every key made here is a throwaway: derived from a label or generated at random for the test, never stored."""
import copy
import hashlib
import json
import os
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import ed25519_ref as ed25519  # noqa: E402
import gen_image_format2_vectors as maker  # noqa: E402
import image_header as ih  # noqa: E402
import image_header2 as h2  # noqa: E402
import image_trust as tr  # noqa: E402

DOC = json.loads((ROOT / "tests" / "image_format2_vectors.json").read_text(encoding="utf-8"))
HAND_WRITTEN = ["ok", "ok", "unknown_key", "unknown_key", "unknown_key", "ok", "rollback_counter", "bad_signature", "bad_signature", "bad_signature",
                "unknown_key", "bad_signature", "unknown_format", "hardware_mismatch", "link_mismatch", "rollback_version", "rollback_version", "ok",
                "rollback_version", "rollback_counter", "too_short", "size_mismatch", "bad_signature", "ok"]


class VectorTests(unittest.TestCase):
    def test_the_vectors_are_current(self):
        self.assertEqual(maker.main(["--check"]), 0)

    def test_every_decision_case_gets_its_verdict(self):
        verdicts = [c["expected"] for c in DOC["decision_cases"]]
        self.assertEqual(len(verdicts), len(DOC["decision_cases"]))
        self.assertEqual(verdicts, HAND_WRITTEN)
        for c in DOC["decision_cases"]:
            got = h2.decide2(bytes.fromhex(c["blob"]), [bytes.fromhex(k) for k in c["trusted"]], hw_id=c["hw_id"], link_address=c["link_address"],
                             floor=c["floor"], installed_version=tuple(c["installed_version"]), nonce=bytes.fromhex(c["nonce"]), token=bytes.fromhex(c["token"]))
            self.assertEqual(got, c["expected"], c["name"])

    def test_every_table_case(self):
        for c in DOC["table_cases"]:
            data = bytes.fromhex(c["table"])
            if c["valid"]:
                self.assertEqual([k.hex() for k in tr.trusted_keys(tr.parse_table(data))], c["trusted"], c["name"])
            else:
                with self.assertRaises(tr.TrustError, msg=c["name"]):
                    tr.parse_table(data)


class LayoutTests(unittest.TestCase):
    def test_the_header_of_format_2_sits_at_the_documented_offsets(self):
        packed = bytes.fromhex(DOC["header_example"])
        self.assertEqual(len(packed), 64)
        self.assertEqual(packed[:4], b"HYDR")
        self.assertEqual(struct.unpack_from("<H", packed, 4)[0], 2)
        self.assertEqual(struct.unpack_from("<I", packed, 40)[0], 5)
        self.assertEqual(packed[44:52], tr.key_id(maker.public("current")))
        self.assertEqual(packed[52:], bytes(12))
        self.assertEqual(h2.parse_header2(packed).pack(), packed)

    def test_the_first_40_bytes_are_those_of_format_1_except_the_format_field(self):
        packed = bytes.fromhex(DOC["header_example"])
        header = h2.parse_header2(packed)
        one = ih.ImageHeader(header.hw_id, *header.version, header.image_size, header.link_address, header.vector_sp, header.vector_reset, header.image_crc32).pack()
        self.assertEqual(packed[:4] + packed[6:40], one[:4] + one[6:40])

    def test_the_domains_differ(self):
        self.assertNotEqual(h2.IMAGE_DOMAIN_2, ih.IMAGE_DOMAIN)
        self.assertEqual(bytes.fromhex(DOC["domain_2"]), h2.IMAGE_DOMAIN_2)

    def test_the_reserved_bytes_must_be_zero_and_a_format_1_header_is_refused(self):
        packed = bytes.fromhex(DOC["header_example"])
        for offset, value in ((6, 1), (18, 1), (52, 1), (63, 1), (4, 1)):
            broken = bytearray(packed)
            broken[offset] = value
            with self.assertRaises(ih.ImageError):
                h2.parse_header2(bytes(broken))

    def test_the_table_has_56_and_100_bytes(self):
        self.assertEqual((tr.table_size(1), tr.table_size(2)), (56, 100))


class EphemeralKeyTests(unittest.TestCase):
    def test_a_random_key_signs_and_only_its_own_table_trusts_it(self):
        seed = os.urandom(32)   # ephemeral: never written anywhere
        body = maker.image(5)
        header = h2.make_header2(seed, body, maker.HW_ID, (0, 3, 0), maker.LINK, 1)
        blob = header.pack() + body + h2.sign_image2(seed, header, body)
        mine, other = ed25519.public_key(seed), ed25519.public_key(os.urandom(32))
        kwargs = {"hw_id": maker.HW_ID, "link_address": maker.LINK, "floor": 0, "installed_version": (0, 2, 0)}
        self.assertEqual(h2.decide2(blob, [mine], **kwargs), "ok")
        self.assertEqual(h2.decide2(blob, [other], **kwargs), "unknown_key")

    def test_the_signer_refuses_a_header_that_names_another_key(self):
        body = maker.image(5)
        header = h2.make_header2(maker.seed("current"), body, maker.HW_ID, (0, 3, 0), maker.LINK, 1)
        with self.assertRaises(ih.ImageError):
            h2.sign_image2(maker.seed("next"), header, body)


class SigningStepOutputTests(unittest.TestCase):
    """What the signing step writes for an image (<image>.hdr2 and <image>.sig2 next to <image>.bin) is accepted by the decision a bootloader takes.
    The bytes are fixed in tools/gen_image_format2_vectors.py (a throwaway key), so a change of the reference that would make a bootloader refuse
    the files of the signing step shows here before anything is signed for real."""

    seed = hashlib.sha256(b"hydra-umc throwaway test key signing tool output").digest()
    image = bytes.fromhex(maker.TOOL_IMAGE)
    hdr2 = bytes.fromhex(maker.TOOL_HDR2)
    sig2 = bytes.fromhex(maker.TOOL_SIG2)
    public = bytes.fromhex(maker.TOOL_PUBLIC)

    def decide(self, blob, **over):
        args = {"hw_id": maker.HW_ID, "link_address": maker.LINK, "floor": 0, "installed_version": (0, 2, 2)}
        args.update(over)
        return h2.decide2(blob, [self.public], **args)

    def test_the_fixed_bytes_are_what_the_reference_makes_for_the_same_inputs(self):
        header = h2.make_header2(self.seed, self.image, maker.HW_ID, (0, 3, 0), maker.LINK, 7)
        self.assertEqual(header.pack(), self.hdr2)
        self.assertEqual(h2.sign_image2(self.seed, header, self.image), self.sig2)
        self.assertEqual(ed25519.public_key(self.seed), self.public)

    def test_the_image_is_one_the_c_bootloader_also_accepts(self):
        """The C decision (update_verify.h) also wants the reset vector inside the image and the vector words of the header to be the first words
        of the image; the Python reference does not look, so a fixed image that breaks it passes here and fails the host tests of the bootloaders."""
        first, reset = struct.unpack_from("<II", self.image, 0)
        header = h2.parse_header2(self.hdr2)
        self.assertEqual((first, reset), (header.vector_sp, header.vector_reset))
        self.assertGreaterEqual(reset, header.link_address)
        self.assertLess(reset - header.link_address, header.image_size)
        self.assertEqual(header.image_size % 8, 0)

    def test_the_files_have_the_sizes_the_updater_expects(self):
        self.assertEqual((len(self.hdr2), len(self.sig2)), (64, 64))
        self.assertEqual(h2.parse_header2(self.hdr2).image_size, len(self.image))

    def test_header_then_image_then_signature_is_accepted(self):
        self.assertEqual(self.decide(self.hdr2 + self.image + self.sig2), "ok")

    def test_the_rollback_counter_of_the_files_is_the_floor(self):
        self.assertEqual(self.decide(self.hdr2 + self.image + self.sig2, floor=7), "ok")
        self.assertEqual(self.decide(self.hdr2 + self.image + self.sig2, floor=8), "rollback_counter")

    def test_the_files_are_refused_for_another_board_or_a_key_that_is_not_in_the_table(self):
        blob = self.hdr2 + self.image + self.sig2
        self.assertEqual(self.decide(blob, hw_id=0x48374337), "hardware_mismatch")
        self.assertEqual(h2.decide2(blob, [maker.public("current")], hw_id=maker.HW_ID, link_address=maker.LINK, floor=0, installed_version=(0, 2, 2)), "unknown_key")

    def test_any_changed_byte_of_the_three_files_is_refused(self):
        blob = self.hdr2 + self.image + self.sig2
        for index in range(0, len(blob), 7):
            changed = maker.flip(blob, index)
            self.assertNotEqual(self.decide(changed), "ok", index)


class TrustFileTests(unittest.TestCase):
    def setUp(self):
        self.doc = tr.load()

    def test_the_source_file_is_valid_and_holds_no_secret(self):
        tr.validate(self.doc)
        raw = (ROOT / "docs" / "image_trust.json").read_text(encoding="utf-8")
        for word in ("BEGIN PRIVATE", '"seed"'):
            self.assertNotIn(word, raw)

    def test_the_dev_key_is_the_public_development_seed(self):
        entry = self.doc["flavours"]["dev"]["keys"][0]
        self.assertEqual(ed25519.public_key(h2.DEV_SEED).hex(), entry["public"])

    def test_the_release_table_cannot_be_built_until_a_key_is_provisioned(self):
        if not self.doc["flavours"]["release"]["keys"]:
            with self.assertRaises(tr.TrustError):
                tr.table_bytes(self.doc, "release")

    def test_the_targets_are_the_three_bootloaders_with_the_ids_of_the_signing_tool(self):
        import sign_release  # noqa: PLC0415
        ids = {name: int(t["hardware_id"], 16) for name, t in self.doc["targets"].items()}
        self.assertEqual(ids, {"rcb": sign_release.HARDWARE_ID_BY_BOARD["RCB"], "kb_cm7": sign_release.HARDWARE_ID_BY_BOARD["KB_CM7"],
                               "kb_cm4": sign_release.HARDWARE_ID_BY_BOARD["KB_CM4"]})

    def test_a_weaker_policy_or_a_wrong_key_id_is_refused(self):
        bad = copy.deepcopy(self.doc)
        bad["default_policy"] = "either"
        with self.assertRaises(tr.TrustError):
            tr.validate(bad)
        bad = copy.deepcopy(self.doc)
        bad["flavours"]["dev"]["keys"][0]["key_id"] = "00" * 8
        with self.assertRaises(tr.TrustError):
            tr.validate(bad)

    def test_the_binary_check_notices_a_change_in_any_byte_of_the_table(self):
        offset = int(self.doc["targets"]["rcb"]["table_offset"], 16)
        table = tr.table_bytes(self.doc, "dev")
        binary = bytes(offset) + table + bytes(64)
        self.assertIsNone(tr.check_binary(self.doc, binary, "rcb", "dev"))
        for index in range(len(table)):
            changed = bytearray(binary)
            changed[offset + index] ^= 1
            self.assertIsNotNone(tr.check_binary(self.doc, bytes(changed), "rcb", "dev"), index)
        self.assertIsNotNone(tr.check_binary(self.doc, bytes(offset + 100), "rcb", "dev"))

    def test_the_entry_command_prints_only_the_public_half(self):
        import io  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        from contextlib import redirect_stdout  # noqa: PLC0415
        seed = os.urandom(32)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "ephemeral.seed"
            path.write_text(seed.hex() + "\n", encoding="ascii")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(tr.main(["entry", str(path)]), 0)
        self.assertNotIn(seed.hex(), out.getvalue())
        self.assertEqual(json.loads(out.getvalue())["public"], ed25519.public_key(seed).hex())


if __name__ == "__main__":
    unittest.main()
