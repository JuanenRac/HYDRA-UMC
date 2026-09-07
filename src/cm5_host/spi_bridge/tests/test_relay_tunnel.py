# =============================================================================
# HYDRA-UMC - spi_bridge relay_tunnel tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
import struct
import unittest

from spi_bridge.bootloader_client import SpiOtaFlasher, query_version
from spi_bridge.protocol import (
    OFS_DATA,
    OFS_PAGE_ACK,
    OFS_RELAY_RECV,
    OFS_RELAY_SEND,
    OFS_STATUS,
    OFS_VERSION_RESPONSE,
    SPI_TARGET_STACKA,
    STATUS_VERIFY_OK,
    SpiOtaFrame,
)
from spi_bridge.relay_tunnel import (
    URTC_BOOTLOADER_CAN_BASE,
    RelayedTransport,
    RelayFrame,
    build_relay_send_fragments,
    parse_relay_fragment,
)


class FragmentationTests(unittest.TestCase):
    def test_a_short_frame_fits_in_one_fragment(self):
        frame = RelayFrame(URTC_BOOTLOADER_CAN_BASE, 4, b"\xde\xad\xbe\xef")
        fragments = build_relay_send_fragments(frame)
        self.assertEqual(len(fragments), 1)
        can_id, dlc, data = parse_relay_fragment(fragments[0])
        self.assertEqual((can_id, dlc, data), (URTC_BOOTLOADER_CAN_BASE, 4, b"\xde\xad\xbe\xef"))

    def test_an_8_byte_frame_splits_into_exactly_two_fragments(self):
        frame = RelayFrame(URTC_BOOTLOADER_CAN_BASE + 1, 8, bytes(range(8)))
        fragments = build_relay_send_fragments(frame)
        self.assertEqual(len(fragments), 2)
        id1, dlc1, data1 = parse_relay_fragment(fragments[0])
        id2, dlc2, data2 = parse_relay_fragment(fragments[1])
        self.assertEqual((id1, dlc1), (URTC_BOOTLOADER_CAN_BASE + 1, 8))
        self.assertEqual((id2, dlc2), (URTC_BOOTLOADER_CAN_BASE + 1, 8))
        self.assertEqual(data1 + data2, bytes(range(8)))

    def test_round_trip_through_a_real_spi_ota_frame_payload(self):
        # RELAY_SEND/RELAY_RECV fragments travel inside a real SpiOtaFrame's
        # own 8-byte payload field - confirms they actually fit.
        frame = RelayFrame(URTC_BOOTLOADER_CAN_BASE, 8, bytes(range(8)))
        for fragment in build_relay_send_fragments(frame):
            spi_frame = SpiOtaFrame(SPI_TARGET_STACKA, 2, OFS_RELAY_SEND, len(fragment), fragment)
            restored = SpiOtaFrame.from_bytes(spi_frame.to_bytes())
            self.assertEqual(restored.payload, fragment)


class FakeRelayingTier1Transport:
    """A real, faithful stand-in for a Robot Controller Board relaying to a
    simulated URTC Tool Head - queues RELAY_SEND fragments, reassembles a
    real CAN frame once enough bytes arrive, runs the exact same bootloader
    logic FakeBootloaderTransport uses (kept independent here rather than
    imported, since a real RCB's relay firmware doesn't share code with the
    H745's own SPI-OTA bootloader either), and serves the response back
    fragmented over RELAY_RECV, one fragment per poll - a real, pull-based
    FIFO, not an all-at-once return."""

    def __init__(self, *, hardware_id: int = 0x0303CC01):
        self.hardware_id = hardware_id
        self._inbound_id: int | None = None
        self._inbound_dlc = 0
        self._inbound_data = b""
        self._pending_recv_fragments: list[bytes] = []
        self.pages_received = 0
        self._page_buffer = bytearray()

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        if frame.frame_type == OFS_RELAY_SEND:
            can_id, total_dlc, fragment_data = parse_relay_fragment(frame.payload[: frame.dlc])
            if self._inbound_id != can_id:
                self._inbound_id, self._inbound_dlc, self._inbound_data = can_id, total_dlc, b""
            self._inbound_data += fragment_data
            if len(self._inbound_data) >= self._inbound_dlc:
                self._run_urtc_bootloader_logic(self._inbound_id, self._inbound_dlc, self._inbound_data[: self._inbound_dlc])
                self._inbound_id = None
            return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)

        if frame.frame_type == OFS_RELAY_RECV:
            if not self._pending_recv_fragments:
                return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_RELAY_RECV, 0)
            fragment = self._pending_recv_fragments.pop(0)
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_RELAY_RECV, len(fragment), fragment)

        raise AssertionError(f"unexpected direct frame_type in relay test: {frame.frame_type}")

    def _run_urtc_bootloader_logic(self, can_id: int, dlc: int, data: bytes) -> None:
        offset = can_id - URTC_BOOTLOADER_CAN_BASE
        if offset == 0x08:  # OFS_QUERY_VERSION
            payload = bytes([0x00]) + struct.pack(">I", self.hardware_id) + struct.pack(">H", 0) + bytes([3])
            self._queue_response(URTC_BOOTLOADER_CAN_BASE + OFS_VERSION_RESPONSE, payload)
        elif offset == OFS_DATA:
            self._page_buffer += data
            if len(self._page_buffer) >= 2048 or dlc < 5:
                self._page_buffer.clear()
                self.pages_received += 1
            # no response queued for a bare DATA frame - matches the real protocol
        elif offset == OFS_PAGE_ACK:
            acked_index = max(0, self.pages_received - 1)
            self._queue_response(URTC_BOOTLOADER_CAN_BASE + OFS_PAGE_ACK, struct.pack(">I", acked_index))
        elif offset == OFS_STATUS:
            self._queue_response(URTC_BOOTLOADER_CAN_BASE + OFS_STATUS, bytes([STATUS_VERIFY_OK]))
        # ENTER_BOOTLOADER/START_UPDATE/HMAC_CHUNK/END_UPDATE - accepted silently, no response needed

    def _queue_response(self, can_id: int, data: bytes) -> None:
        response = RelayFrame(can_id, len(data), data)
        self._pending_recv_fragments.extend(build_relay_send_fragments(response))

    def wait_data_ready(self, timeout_seconds: float) -> bool:
        return True

    def close(self) -> None:
        pass


class RelayedTransportTests(unittest.TestCase):
    def setUp(self):
        self.tier1 = FakeRelayingTier1Transport(hardware_id=0x0303CC01)
        self.relayed = RelayedTransport(self.tier1, SPI_TARGET_STACKA, 2)

    def test_query_version_reaches_the_real_urtc_head_through_the_tunnel(self):
        # query_version() is completely unchanged from the Tier 0/1 case -
        # it just receives a RelayedTransport instead of a direct one.
        info = query_version(self.relayed, SPI_TARGET_STACKA, 2)
        self.assertTrue(info.online)
        self.assertEqual(info.hardware_id, 0x0303CC01)

    def test_a_full_flash_cycle_reaches_done_through_the_tunnel(self):
        flasher = SpiOtaFlasher(self.relayed, hmac_key=b"\x00" * 32)
        firmware = b"\xAB" * 2048  # exactly one page
        phases = [progress.phase for progress in flasher.flash(SPI_TARGET_STACKA, 2, 0x0303CC01, firmware, 0, 1)]
        self.assertEqual(phases, ["entering_bootloader", "transferring", "verifying", "done"])

    def test_a_multi_fragment_frame_reassembles_correctly_through_the_tunnel(self):
        # END_UPDATE's real 8-byte payload (crc32 + version) needs 2 real
        # RELAY_SEND fragments each way - proves fragmentation/reassembly
        # survives a real 8-byte round trip, not just short frames.
        flasher = SpiOtaFlasher(self.relayed, hmac_key=b"\x11" * 32)
        firmware = b"\xCD" * 100
        phases = [progress.phase for progress in flasher.flash(SPI_TARGET_STACKA, 2, 0x0303CC01, firmware, 2, 5)]
        self.assertEqual(phases[-1], "done")

    def test_a_relay_timeout_fails_closed_not_an_infinite_hang(self):
        class NeverRespondingTier1(FakeRelayingTier1Transport):
            def transceive(self, frame):
                if frame.frame_type == OFS_RELAY_RECV:
                    return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_RELAY_RECV, 0)
                return super().transceive(frame)

        relayed = RelayedTransport(NeverRespondingTier1(), SPI_TARGET_STACKA, 2)
        info = query_version(relayed, SPI_TARGET_STACKA, 2)
        self.assertFalse(info.online)


if __name__ == "__main__":
    unittest.main()
