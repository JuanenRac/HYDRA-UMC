# =============================================================================
# HYDRA-UMC - spi_bridge transport tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
import unittest

from spi_bridge.transport import _parse_spidev_path, open_spi_transport


def _spidev_and_gpiod_installed() -> bool:
    try:
        import spidev  # noqa: F401
        import gpiod  # noqa: F401

        return True
    except ImportError:
        return False


class ParseSpidevPathTests(unittest.TestCase):
    def test_real_spidev_path_is_parsed_into_bus_and_device(self):
        self.assertEqual(_parse_spidev_path("/dev/spidev0.0"), (0, 0))
        self.assertEqual(_parse_spidev_path("/dev/spidev1.2"), (1, 2))

    def test_a_malformed_path_is_rejected(self):
        for bad in ("/dev/ttyUSB0", "spidev0", "/dev/spidev0"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    _parse_spidev_path(bad)


class OpenSpiTransportTests(unittest.TestCase):
    def test_missing_spidev_or_gpiod_raises_a_clear_runtime_error_not_an_import_error(self):
        if _spidev_and_gpiod_installed():
            self.skipTest("spidev/gpiod are installed in this environment - nothing to prove here")
        with self.assertRaises(RuntimeError) as context:
            open_spi_transport("/dev/spidev0.0", "/dev/gpiochip0", 3)
        self.assertIn("spidev/gpiod are not installed", str(context.exception))


if __name__ == "__main__":
    unittest.main()
