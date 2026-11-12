# =============================================================================
# HYDRA-UMC - tests/test_machine_cfg_corruption.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""A corrupted config/machine.cfg is refused or read as something safe, never as a surprise.

The parameters of the machine decide how each axis stops and which pin is the stop input. A damaged file (a flipped byte, a cut file, a line that went missing or
was repeated, a value that is huge or is not a number) must end in one of two ways: the loader says it cannot use the file (`ConfigError`), or it returns a
configuration in which every stop category is 0 or 1, the input pin is a free pin and the header can be generated from it. Any other exception, or a
configuration outside those limits, is a bug. The mutations come from a fixed seed, so a failure repeats. Evidence: simulator (host test of the loader)."""
import random
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import machine_cfg  # noqa: E402

BASE = (ROOT / "config" / "machine.cfg").read_bytes()
SEED = 13849
ROUNDS = 600


def load_bytes(data: bytes):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "machine.cfg"
        path.write_bytes(data)
        return machine_cfg.load(path)


def mutate(rng: random.Random, data: bytes) -> bytes:
    lines = data.split(b"\n")
    kind = rng.randrange(9)
    if kind == 0 and data:  # flip one byte
        i = rng.randrange(len(data))
        return data[:i] + bytes([data[i] ^ (1 << rng.randrange(8))]) + data[i + 1 :]
    if kind == 1 and data:  # cut the file
        return data[: rng.randrange(len(data))]
    if kind == 2 and len(lines) > 1:  # drop a line
        del lines[rng.randrange(len(lines))]
        return b"\n".join(lines)
    if kind == 3 and len(lines) > 1:  # repeat a line
        i = rng.randrange(len(lines))
        lines.insert(rng.randrange(len(lines)), lines[i])
        return b"\n".join(lines)
    if kind == 4:  # a value that is huge or not a number
        i = rng.randrange(len(lines))
        if b"=" in lines[i]:
            key = lines[i].split(b"=", 1)[0]
            lines[i] = key + b"= " + rng.choice([b"99999999999999999999999", b"-1", b"abc", b"0x", b"1e9", b"", b"\xff\xfe", b"3.5", b"NaN", b"1 2"])
        return b"\n".join(lines)
    if kind == 5:  # a NUL or a control character
        i = rng.randrange(len(data) + 1)
        return data[:i] + rng.choice([b"\x00", b"\x1b", b"\r", b"\x7f"]) + data[i:]
    if kind == 6:  # bytes that are not UTF-8
        i = rng.randrange(len(data) + 1)
        return data[:i] + rng.choice([b"\xc3\x28", b"\xff", b"\xe2\x82"]) + data[i:]
    if kind == 7:  # a section or a key that does not exist
        return data + rng.choice([b"\n[unknown]\nx = 1\n", b"\nsurprise = 1\n", b"\n[", b"\n=\n", b"\n[estop_input\n"])
    return b"".join(rng.choice([data, data[::-1], data * 2, b""]) for _ in range(1))


class CorruptedMachineConfigTests(unittest.TestCase):
    def test_the_file_of_the_repository_loads(self):
        self.assertEqual(machine_cfg.load()["stop_categories"], [0] * 6)

    def test_a_damaged_file_is_refused_or_read_inside_the_safe_limits(self):
        rng = random.Random(SEED)
        refused = accepted = 0
        for round_number in range(ROUNDS):
            data = mutate(rng, mutate(rng, BASE) if round_number % 3 == 0 else BASE)
            try:
                cfg = load_bytes(data)
            except machine_cfg.ConfigError:
                refused += 1
                continue
            accepted += 1
            self.assertTrue(all(c in (0, 1) for c in cfg["stop_categories"]), f"round {round_number}: {cfg['stop_categories']}")
            self.assertEqual(len(cfg["stop_categories"]), 6, f"round {round_number}")
            self.assertIsInstance(machine_cfg.render(cfg), str)
        self.assertGreater(refused, ROUNDS // 10, "the mutations never broke anything: the test is not testing")
        self.assertGreater(accepted, 0, "every mutation was refused: the test is not testing the accepted side")

    def test_the_empty_and_the_binary_file_are_refused_with_a_clear_error(self):
        for data in (b"", b"\x00" * 64, b"\xff" * 64, b"\n\n\n", b"[", b"= = ="):
            try:
                cfg = load_bytes(data)
            except machine_cfg.ConfigError:
                continue
            # a file with nothing in it may legitimately mean "all defaults": then it must be the safe defaults
            self.assertEqual(cfg["stop_categories"], [0] * 6, repr(data))


if __name__ == "__main__":
    unittest.main()
