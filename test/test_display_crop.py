#!/usr/bin/env python3
"""Native pixel/guard regressions for the real display.c renderer.

This is not a target-ABI or on-disc-header test: opera-boot's unsigned long
aliases and VideoImage use the host ABI (64-bit longs and a 32-byte header
on x86-64, versus 32-bit longs and a 24-byte header on ARM). Small dimensions
exercise the same bounded numeric crop path. The native division adapter
provides the target's arithmetic result without compiling ARM assembly or
using util.c's explicitly 32-bit shift loop with 64-bit host longs.

Run with: python3 -m unittest discover -s test -p 'test_display_crop.py'
"""

import ctypes
import mmap
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


_ROOT = Path(__file__).resolve().parents[1]
_SCREEN_WIDTH = 320
_SCREEN_HEIGHT = 240
_VISIBLE_BYTES = _SCREEN_WIDTH * _SCREEN_HEIGHT * 2
_FRAMEBUFFER_BYTES = 256 * 1024
_FRAMEBUFFER_ADDRESS = 0x002C0000
_MADAM_ADDRESS = 0x03300000
_MAP_FIXED_NOREPLACE = 0x100000
_GUARD_PAIR = struct.pack('=HH', 0x1234, 0xABCD)

_DIVISION_ADAPTER = r'''
#include "opera-boot.h"

#include "assert.h"

u32
ob_udiv(u32 n_, u32 d_)
{
  assert(d_ != 0);
  return (n_ / d_);
}
'''


class VideoImage(ctypes.Structure):
    _fields_ = [
        ('version', ctypes.c_ubyte),
        ('pattern', ctypes.c_ubyte * 7),
        ('size', ctypes.c_ulong),
        ('height', ctypes.c_ushort),
        ('width', ctypes.c_ushort),
        ('depth', ctypes.c_ubyte),
        ('image_type', ctypes.c_ubyte),
        ('reserved1', ctypes.c_ubyte),
        ('reserved2', ctypes.c_ubyte),
        ('reserved3', ctypes.c_ulong),
    ]


def _render_native(library_path, width, height):
    """Map hardware addresses only inside an isolated worker process."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.mmap.restype = ctypes.c_void_p
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    libc.munmap.restype = ctypes.c_int
    mappings = []
    try:
        for address, length in ((_MADAM_ADDRESS, mmap.PAGESIZE),
                                (_FRAMEBUFFER_ADDRESS, _FRAMEBUFFER_BYTES)):
            mapped = libc.mmap(address, length, mmap.PROT_READ | mmap.PROT_WRITE,
                               mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS |
                               _MAP_FIXED_NOREPLACE, -1, 0)
            if mapped == ctypes.c_void_p(-1).value:
                error = ctypes.get_errno()
                raise OSError(error, os.strerror(error))
            mappings.append((mapped, length))
            if mapped != address:
                raise RuntimeError('kernel did not honor MAP_FIXED_NOREPLACE')

        ctypes.c_ulong.from_address(_MADAM_ADDRESS + 4).value = 0x29
        initial = _GUARD_PAIR * (_FRAMEBUFFER_BYTES // len(_GUARD_PAIR))
        ctypes.memmove(_FRAMEBUFFER_ADDRESS, initial, len(initial))

        payload = sys.stdin.buffer.read()
        image_bytes = ctypes.sizeof(VideoImage) + len(payload)
        storage = ctypes.create_string_buffer(image_bytes)
        image = VideoImage.from_buffer(storage)
        image.pattern[:] = b'APPSCRN'
        image.size = image_bytes
        image.width = width
        image.height = height
        image.depth = 16
        ctypes.memmove(ctypes.addressof(storage) + ctypes.sizeof(VideoImage),
                       payload, len(payload))
        pattern = (ctypes.c_ubyte * 7).from_buffer_copy(b'APPSCRN')
        library = ctypes.CDLL(library_path)
        library.display_image.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                          ctypes.POINTER(ctypes.c_ubyte)]
        library.display_image.restype = ctypes.c_int
        if library.display_image(storage, image_bytes, pattern) != 1:
            raise RuntimeError('renderer rejected the valid synthetic APPSCRN')
        sys.stdout.buffer.write(ctypes.string_at(_FRAMEBUFFER_ADDRESS,
                                                _FRAMEBUFFER_BYTES))
    finally:
        for address, length in reversed(mappings):
            if libc.munmap(address, length) != 0:
                error = ctypes.get_errno()
                raise OSError(error, os.strerror(error))


def _pixel(x, y, width):
    # Different edge colors expose either crop direction; row-dependent values
    # expose swapped pair lanes, incorrect source stride, and pair wrapping.
    if x == 0:
        return 0x1000 + y
    if x == 1:
        return 0x2000 + y
    if x == width - 2:
        return 0x3000 + y
    if x == width - 1:
        return 0x4000 + y
    return 1 + ((y * 131 + x * 17) % 0x7FFF)


class DisplayCropTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform != 'linux':
            raise unittest.SkipTest('native MMIO regression requires Linux mmap')
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        if not compiler or shutil.which(compiler[0]) is None:
            raise unittest.SkipTest('native MMIO regression requires a C compiler')
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.library_path = str(Path(directory.name) / 'display.so')
        subprocess.run(
            compiler + ['-shared', '-fPIC', '-O0', '-std=gnu89', '-I', str(_ROOT / 'src'),
                        str(_ROOT / 'src' / 'display.c'), '-x', 'c', '-',
                        '-o', cls.library_path],
            input=_DIVISION_ADAPTER, text=True, capture_output=True, check=True)

    def render(self, width, height):
        # Source pairs are complete even when the declared height is odd.
        payload = b''.join(
            struct.pack('=H', _pixel(x, y, width))
            for pair in range((height + 1) // 2)
            for x in range(width)
            for y in (pair * 2, pair * 2 + 1))
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), '--render',
             self.library_path, str(width), str(height)],
            input=payload, capture_output=True, check=True, timeout=30)
        self.assertEqual(len(result.stdout), _FRAMEBUFFER_BYTES)
        self.assertEqual(result.stdout[_VISIBLE_BYTES:],
                         _GUARD_PAIR * ((_FRAMEBUFFER_BYTES - _VISIBLE_BYTES) //
                                        len(_GUARD_PAIR)),
                         'renderer modified the off-visible framebuffer pad')
        return struct.unpack('=' + 'H' * (_VISIBLE_BYTES // 2),
                             result.stdout[:_VISIBLE_BYTES])

    def assert_pixels(self, actual, width, source_left, destination_left,
                      destination_top, drawn_width, drawn_height):
        # Derive expected pixels by screen coordinates, then address the
        # hardware's alternating row lanes independently of the source writer.
        for y in range(_SCREEN_HEIGHT):
            for x in range(_SCREEN_WIDTH):
                expected = 0
                if (destination_top <= y < destination_top + drawn_height and
                        destination_left <= x < destination_left + drawn_width):
                    expected = _pixel(x - destination_left + source_left,
                                      y - destination_top, width)
                offset = (y // 2) * (_SCREEN_WIDTH * 2) + x * 2 + (y % 2)
                if actual[offset] != expected:
                    self.fail('pixel (%d, %d): got 0x%04X, expected 0x%04X' %
                              (x, y, actual[offset], expected))

    def test_oversized_widths_crop_without_wrapping_or_overwriting_guards(self):
        for width, source_left in ((321, 0), (322, 1)):
            with self.subTest(width=width):
                self.assert_pixels(self.render(width, _SCREEN_HEIGHT), width,
                                   source_left, 0, 0, _SCREEN_WIDTH, _SCREEN_HEIGHT)

    def test_full_width_control_keeps_both_edges_and_line_pairs(self):
        self.assert_pixels(self.render(_SCREEN_WIDTH, _SCREEN_HEIGHT),
                           _SCREEN_WIDTH, 0, 0, 0, _SCREEN_WIDTH, _SCREEN_HEIGHT)

    def test_narrow_odd_height_control_keeps_centering_and_complete_final_pair(self):
        # Horizontal half(320 - 317) truncates to 1; vertical half(240 - 235)
        # yields the even margin 2. The 235th row still draws its paired row.
        self.assert_pixels(self.render(317, 235), 317, 0, 1, 2, 317, 236)


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--render':
        _render_native(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    else:
        unittest.main()
