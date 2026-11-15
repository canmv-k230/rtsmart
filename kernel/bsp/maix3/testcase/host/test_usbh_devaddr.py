#!/usr/bin/env python3
"""Exercise CherryUSB's actual address allocator, including occupied hints."""

import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


CHERRYUSB = Path(__file__).resolve().parents[2] / "components/CherryUSB"

# Source coupling: the allocator is cut out of usbh_core.c between these two
# function definitions, and the map struct is taken from usbh_core.h. If a
# rename or reorder breaks an anchor, setUpClass fails saying which one.
ALLOC_START = re.compile(r"^[^\S\n]*(?:static\s+)?(?:inline\s+)?int\s+usbh_allocate_devaddr\s*\(", re.M)
ALLOC_END = re.compile(r"^[^\S\n]*(?:static\s+)?(?:inline\s+)?int\s+__usbh_free_devaddr\s*\(", re.M)
MAP_STRUCT = re.compile(r"^struct\s+usbh_devaddr_map\s*\{.*?^\};", re.M | re.S)


def extract(pattern, text, what, start=0):
    match = pattern.search(text, start)
    if match is None:
        raise AssertionError(f"could not find {what}; update the anchor in {Path(__file__).name}")
    return match


class AddressMap(ctypes.Structure):
    # Mirrors struct usbh_devaddr_map; the generated C checks the layout.
    _fields_ = [("next", ctypes.c_uint8), ("alloctab", ctypes.c_uint32 * 4)]


class DeviceAddressTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("C compiler required")
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tempdir.cleanup)
        tmp = Path(cls.tempdir.name)
        source = (CHERRYUSB / "core/usbh_core.c").read_text()
        header = (CHERRYUSB / "core/usbh_core.h").read_text()
        start = extract(ALLOC_START, source, "usbh_allocate_devaddr() in usbh_core.c")
        end = extract(ALLOC_END, source, "__usbh_free_devaddr() after the allocator in usbh_core.c",
                      start.end())
        struct = extract(MAP_STRUCT, header, "struct usbh_devaddr_map in usbh_core.h").group(0)
        function = re.sub(r"^\s*static\s+", "", source[start.start():end.start()], count=1)
        layout = "".join(
            f"_Static_assert(offsetof(struct usbh_devaddr_map, {name}) == {field.offset}, "
            f"\"AddressMap.{name} offset differs from usbh_core.h\");\n"
            for name, _ in AddressMap._fields_ for field in [getattr(AddressMap, name)])
        (tmp / "allocator.c").write_text(
            "#include <stddef.h>\n#include <stdint.h>\n"
            "#define USB_ERR_NOMEM 1\n"
            + struct + "\n"
            + f"_Static_assert(sizeof(struct usbh_devaddr_map) == {ctypes.sizeof(AddressMap)}, "
              "\"AddressMap size differs from usbh_core.h\");\n"
            + layout + function)
        build = subprocess.run([compiler, "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                                str(tmp / "allocator.c"), "-o", str(tmp / "allocator.so")],
                               capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("extracted allocator failed to compile:\n" + build.stderr)
        cls.lib = ctypes.CDLL(str(tmp / "allocator.so"))
        cls.lib.usbh_allocate_devaddr.argtypes = [ctypes.POINTER(AddressMap)]
        cls.lib.usbh_allocate_devaddr.restype = ctypes.c_int

    def allocate(self, addrmap):
        return self.lib.usbh_allocate_devaddr(ctypes.byref(addrmap))

    def test_fresh_bus(self):
        addrmap = AddressMap(next=2)
        self.assertEqual(self.allocate(addrmap), 2)
        self.assertEqual(addrmap.next, 3)
        self.assertEqual(addrmap.alloctab[0], 1 << 2)

    def test_skip_occupied_hint_after_reenumeration(self):
        addrmap = AddressMap(next=6)
        addrmap.alloctab[0] = sum(1 << addr for addr in range(2, 7))
        self.assertEqual(self.allocate(addrmap), 7)
        self.assertEqual(addrmap.next, 8)
        self.assertEqual(addrmap.alloctab[0], sum(1 << addr for addr in range(2, 8)))

    def test_search_across_bitmap_words(self):
        addrmap = AddressMap(next=30)
        addrmap.alloctab[0] = 0xffffffff
        addrmap.alloctab[1] = 0x7
        self.assertEqual(self.allocate(addrmap), 35)
        self.assertEqual(addrmap.alloctab[1], 0xf)

    def test_wrap_skips_reserved_and_occupied_addresses(self):
        addrmap = AddressMap(next=127)
        addrmap.alloctab[3] = 1 << 31
        addrmap.alloctab[0] = (1 << 2) | (1 << 3)
        self.assertEqual(self.allocate(addrmap), 4)

    def test_last_free_address_after_wrap(self):
        addrmap = AddressMap(next=65)
        for index in range(4):
            addrmap.alloctab[index] = 0xffffffff
        addrmap.alloctab[0] &= ~(1 << 5)
        self.assertEqual(self.allocate(addrmap), 5)
        self.assertEqual(self.allocate(addrmap), -1)

    def test_exhaustion_does_not_allocate_reserved_addresses(self):
        addrmap = AddressMap(next=2)
        addresses = [self.allocate(addrmap) for _ in range(126)]
        self.assertEqual(addresses, list(range(2, 128)))
        self.assertEqual(self.allocate(addrmap), -1)


if __name__ == "__main__":
    unittest.main()
