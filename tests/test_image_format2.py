"""Header format 2, the trust table and its public source file: the host reference held to the shared vectors and to hand-written verdicts.
Every key made here is a throwaway: derived from a label or generated at random for the test, never stored."""
import copy
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
                "rollback_version", "rollback_counter", "too_short", "size_mismatch", "bad_signature"]


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
