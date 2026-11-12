# =============================================================================
# HYDRA-UMC - tests/test_ui_link.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The frames between the H757 and the Jetson for the interface of the display (src/ui_h757/spec/jetson_link.json, tools/ui_link.py): the status is the heartbeat of the
view-model, the camera picture is reassembled from chunks, and damage, silence and a hang in the middle of a picture cost only what they should. Evidence: simulator."""
import json
import random
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import spi_boot_master as boot  # noqa: E402
import ui_budget  # noqa: E402
import ui_link as link  # noqa: E402
import ui_model as model  # noqa: E402


class FramesTests(unittest.TestCase):
    def test_the_types_do_not_collide_with_those_of_the_boot_exchange(self):
        mine = link.MASTER_TYPES | link.HOST_TYPES
        theirs = boot.MASTER_TYPES | boot.HOST_TYPES
        self.assertEqual(mine & theirs, set())
        self.assertEqual({f["name"] for f in link.CONFIG["frames"]}, {"STATUS_REQ", "SNAPSHOT_REQ", "STATUS", "SNAPSHOT", "SNAPSHOT_NONE"})

    def test_the_frame_layout_in_the_data_is_the_one_the_code_packs(self):
        for name, fmt in (("STATUS", link.STATUS_FORMAT), ("SNAPSHOT_REQ", link.SNAPSHOT_REQ_FORMAT)):
            fields = [f["format"] for f in link.FRAMES[name]["payload"]]
            self.assertEqual("<" + "".join(fields), fmt, name)
        self.assertEqual("<" + "".join(f["format"] for f in link.FRAMES["SNAPSHOT"]["payload"][:3]), link.SNAPSHOT_HEADER)
        self.assertEqual(link.CONFIG["framing"]["sync"], boot.SYNC)
        self.assertLessEqual(link.CHUNK + struct.calcsize(link.SNAPSHOT_HEADER), link.CONFIG["framing"]["max_payload"])

    def test_a_status_round_trips(self):
        frame = link.encode_status(2, 23, 52.3, True, 3600, sequence=7)
        status = link.decode_status(frame, 7)
        self.assertEqual((status.zone_level, status.cpu_pct, status.temperature_c, status.picture_available, status.uptime_s), (2, 23, 52.3, True, 3600))

    def test_a_damaged_or_foreign_status_is_never_believed(self):
        frame = link.encode_status(0, 10, 40.0, False, 5, sequence=9)
        for index in range(len(frame)):
            damaged = bytearray(frame)
            damaged[index] ^= 0x01
            with self.assertRaises(link.LinkError, msg=str(index)):
                link.decode_status(bytes(damaged), 9)
        with self.assertRaises(link.LinkError):
            link.decode_status(frame, 10)  # an answer for another request
        with self.assertRaises(link.LinkError):
            link.decode_status(frame[:-3], 9)
        bad_level = boot.encode(link.T_STATUS, 9, struct.pack(link.STATUS_FORMAT, link.STATUS_VERSION, 9, 10, 400, 0, 5))
        with self.assertRaises(link.LinkError):
            link.decode_status(bad_level, 9)
        other_version = boot.encode(link.T_STATUS, 9, struct.pack(link.STATUS_FORMAT, 2, 0, 10, 400, 0, 5))
        with self.assertRaises(link.LinkError):
            link.decode_status(other_version, 9)


class AssemblerTests(unittest.TestCase):
    def setUp(self):
        self.picture = link.make_jpeg(1000)

    def chunks(self, picture_id=1):
        return [(picture_id, len(self.picture), off, self.picture[off : off + link.CHUNK]) for off in range(0, len(self.picture), link.CHUNK)]

    def test_in_order_chunks_give_the_picture_and_only_at_the_end(self):
        a = link.Assembler(4096)
        parts = self.chunks()
        for part in parts[:-1]:
            self.assertIsNone(a.feed(*part))
        self.assertEqual(a.feed(*parts[-1]), self.picture)

    def test_a_gap_a_repeat_a_wrong_total_or_a_picture_without_markers_drops_it(self):
        parts = self.chunks()
        a = link.Assembler(4096)
        a.feed(*parts[0])
        self.assertIsNone(a.feed(*parts[2]))  # a gap
        self.assertEqual(a.dropped, 1)
        a.feed(*parts[0])
        self.assertIsNone(a.feed(*parts[0]))  # a repeat
        self.assertEqual(a.dropped, 2)
        self.assertIsNone(a.feed(1, 99999, 0, b"\xff\xd8"))  # a total above the limit
        a2 = link.Assembler(4096)
        a2.feed(*parts[0])
        self.assertIsNone(a2.feed(2, len(self.picture), parts[1][2], parts[1][3]))  # another picture that does not start at zero
        broken = bytearray(self.picture)
        broken[-1] = 0
        a3 = link.Assembler(4096)
        result = None
        for off in range(0, len(broken), link.CHUNK):
            result = a3.feed(5, len(broken), off, bytes(broken[off : off + link.CHUNK]))
        self.assertIsNone(result)
        self.assertEqual(a3.dropped, 1)

    def test_nothing_but_the_whole_picture_is_ever_returned_even_with_random_loss_and_order(self):
        rng = random.Random(4242)
        for _ in range(300):
            a = link.Assembler(4096)
            parts = self.chunks()
            if rng.random() < 0.5:
                rng.shuffle(parts)
            for part in parts:
                if rng.random() < 0.9:
                    picture = a.feed(*part)
                    if picture is not None:
                        self.assertEqual(picture, self.picture)


class MasterTests(unittest.TestCase):
    def run_link(self, jetson, seconds=3.0, step=50, bound=None):
        master = link.LinkMaster(jetson, bound or 2 * 24576)
        events = []
        for now in range(0, int(seconds * 1000), step):
            events += [(now, name, args) for name, args in master.step(now)]
        return master, events

    def test_a_good_jetson_gives_heartbeats_with_its_level_and_one_complete_picture(self):
        picture = link.make_jpeg(24576)
        master, events = self.run_link(link.FakeJetson(picture, zone_level=1))
        self.assertEqual(master.pictures, [picture])
        self.assertEqual({name for _, name, _ in events}, {"HB", "SPI_OK"})
        self.assertTrue(all(args == (1,) for _, name, args in events if name == "HB"))
        self.assertEqual(master.assembler.dropped, 0)

    def test_a_newer_picture_is_fetched_and_an_old_one_is_not_fetched_again(self):
        jetson = link.FakeJetson(link.make_jpeg(5000, 1))
        master = link.LinkMaster(jetson, 2 * 24576)
        for now in range(0, 2000, 50):
            master.step(now)
        self.assertEqual(len(master.pictures), 1)
        jetson.next_picture(link.make_jpeg(5000, 2))
        for now in range(2000, 4000, 50):
            master.step(now)
        self.assertEqual(len(master.pictures), 2)
        self.assertNotEqual(master.pictures[0], master.pictures[1])

    def test_a_silent_jetson_drives_the_model_to_degraded_and_a_return_brings_it_back(self):
        jetson = link.FakeJetson(link.make_jpeg(2000))
        master = link.LinkMaster(jetson, 2 * 24576)
        state = model.State(model.load_config())
        state._derive()
        for name, args in master.step(0):
            state.event(0, name, *args)
        self.assertEqual(model.MODES[state.mode], "normal")
        jetson.mode = "silent"
        for now in range(50, 3000, 50):
            for name, args in master.step(now):
                state.event(now, name, *args)
        self.assertEqual(model.MODES[state.mode], "degraded")
        self.assertEqual(model.LINKS[state.link], "spi_fault")  # no answer at all for this long is told as a link fault, as the model decides
        jetson.mode = "ok"
        for now in range(3000, 3300, 50):
            for name, args in master.step(now):
                state.event(now, name, *args)
        self.assertEqual(model.MODES[state.mode], "normal")

    def test_a_corrupting_link_is_never_a_heartbeat(self):
        jetson = link.FakeJetson(link.make_jpeg(2000))
        jetson.mode = "corrupt"
        master, events = self.run_link(jetson)
        self.assertNotIn("HB", {name for _, name, _ in events})
        self.assertEqual(master.pictures, [])

    def test_a_hang_in_the_middle_of_a_picture_keeps_nothing_half_built(self):
        jetson = link.FakeJetson(link.make_jpeg(24576))
        jetson.mode = "hang_in_picture"
        master = link.LinkMaster(jetson, 2 * 24576)
        for now in range(0, 1000, 50):
            master.step(now)
        self.assertEqual(master.pictures, [])
        self.assertEqual(master.assembler.data, bytearray())

    def test_the_picture_traffic_fits_the_share_of_the_link_the_budget_allows(self):
        inputs = ui_budget.load_inputs()
        size = int(inputs["camera_thumb_jpeg_bytes"])
        picture = link.make_jpeg(size)
        master = link.LinkMaster(link.FakeJetson(picture), 2 * size)
        master.step(0)  # the first status and the whole picture
        self.assertEqual(master.pictures[0], picture)
        status_bytes = len(boot.encode(link.T_STATUS_REQ, 1)) + len(link.encode_status(0, 0, 0.0, True, 0, 1))
        picture_bytes = master.wire_bytes - status_bytes
        share = picture_bytes * 8 / inputs["spi_link_clock_hz"] * inputs["camera_thumb_fps"]
        self.assertLess(share, inputs["spi_thumb_max_fraction"])  # with the framing included, not just the picture bytes
        self.assertGreater(picture_bytes, size)  # the framing costs something
        self.assertLess(picture_bytes, 1.3 * size)


class ContractTests(unittest.TestCase):
    def test_the_spec_points_the_signals_of_the_jetson_at_these_frames(self):
        spec = json.loads((ROOT / "src" / "ui_h757" / "spec" / "ui_spec.json").read_text(encoding="utf-8"))
        refs = {s["id"]: s["contract_ref"] for s in spec["signals"] if s["needs_jetson"]}
        self.assertEqual(set(refs.values()), {"link:STATUS", "link:SNAPSHOT"})
        self.assertEqual(refs["jetson.camera_snapshot"], "link:SNAPSHOT")
        self.assertEqual(refs["jetson.safety_level"], "link:STATUS")
        self.assertNotIn("jetson_status", spec["pending_contracts"])
        self.assertNotIn("jetson_snapshot", spec["pending_contracts"])
        for ref in refs.values():
            self.assertIn(ref.split(":")[1], link.FRAMES)

    def test_the_polling_period_fits_the_timeout_of_the_view_model(self):
        cfg = model.load_config()
        self.assertLess(link.LIMITS["status_period_ms"] * 3, cfg["jetson_timeout_ms"])  # three lost polls still leave time before the degraded mode


if __name__ == "__main__":
    unittest.main()
