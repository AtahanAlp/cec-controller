import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('maintenance', Path(__file__).parents[2]/'tools/pico-maintenance.py')
maintenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maintenance)


def block(address=0x10000000, family=0xe48bff56, number=0, total=1):
    data = bytearray(512)
    struct.pack_into('<8I', data, 0, 0x0a324655, 0x9e5d5157, 0x2000, address, 256, number, total, family)
    struct.pack_into('<I', data, 508, 0x0ab16f30)
    return bytes(data)


class FlashGuards(unittest.TestCase):
    def validate(self, data):
        maintenance.validate_uf2(data, hashlib.sha256(data).hexdigest())

    def test_close_after_expected_usb_disconnect(self):
        console = maintenance.Console.__new__(maintenance.Console)
        console.fd = 99
        console.exclusive = True
        console.original = []
        with mock.patch.object(maintenance.termios, 'tcsetattr', side_effect=maintenance.termios.error(5, 'device disconnected')), \
                mock.patch.object(maintenance.fcntl, 'ioctl', side_effect=OSError(5, 'device disconnected')), \
                mock.patch.object(maintenance.os, 'close') as close:
            console.close()
            close.assert_called_once_with(99)

    def test_valid_image(self):
        self.validate(block())

    def test_checksum(self):
        with self.assertRaises(ValueError):
            maintenance.validate_uf2(block(), '0'*64)

    def test_protected_addresses(self):
        for address in (0, 0x20000000, 0x101fc000, 0x10000001, 0x10000100):
            with self.subTest(address=address), self.assertRaises(ValueError):
                self.validate(block(address=address))

    def test_family_and_framing(self):
        for data in (b'', block()[:-1], block(family=0), block(number=1), block(total=2), block()+block()):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                self.validate(data)

    def test_duplicate_addresses(self):
        with self.assertRaises(ValueError):
            self.validate(block(total=2)+block(total=2, number=1))


if __name__ == '__main__':
    unittest.main()
