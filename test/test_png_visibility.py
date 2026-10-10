#!/usr/bin/env python3
"""Consumer regressions for filtered screenshot visibility and PNG errors."""

import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


_HELPER = Path(__file__).resolve().with_name('png_visibility.py')
_SPEC = importlib.util.spec_from_file_location('png_visibility', _HELPER)
png_visibility = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(png_visibility)


def chunk(kind, payload):
    return (struct.pack('>I', len(payload)) + kind + payload
            + struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))


def png_bytes(width, height, channels, scanlines, depth=8, interlace=0,
              color=None, compressed=None):
    if color is None:
        color = 2 if channels == 3 else 6
    header = struct.pack('>IIBBBBB', width, height, depth, color, 0, 0, interlace)
    if compressed is None:
        compressed = zlib.compress(scanlines)
    # Split IDAT across an arbitrary boundary, including inside the zlib header.
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
            + chunk(b'IDAT', compressed[:1]) + chunk(b'IDAT', compressed[1:])
            + chunk(b'IEND', b''))


def filtered_rows(rows, channels, filters):
    encoded = bytearray()
    previous = bytes(len(rows[0]))
    for row, filtering in zip(rows, filters):
        encoded.append(filtering)
        for index, value in enumerate(row):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            corner = previous[index - channels] if index >= channels else 0
            if filtering == 0:
                predictor = 0
            elif filtering == 1:
                predictor = left
            elif filtering == 2:
                predictor = above
            elif filtering == 3:
                predictor = (left + above) // 2
            else:
                # Select by distance, then PNG's left/above/corner tie priority.
                prediction = left + above - corner
                choices = (left, above, corner)
                choice = min(range(3), key=lambda candidate:
                             (abs(prediction - choices[candidate]), candidate))
                predictor = choices[choice]
            encoded.append((value - predictor) % 256)
        previous = row
    return bytes(encoded)


def sparse_rows(nonzero, channels, height=4):
    width = 400
    rows = [bytearray(width * channels) for _ in range(height)]
    if channels == 4:
        for row in rows:
            row[3::4] = b'\xff' * width
    # 137 is coprime with the RGB sample count, so positions never repeat.
    for sample in range(nonzero):
        position = sample * 137 % (height * width * 3)
        row, rgb_index = divmod(position, width * 3)
        pixel, color = divmod(rgb_index, 3)
        rows[row][pixel * channels + color] = sample * 29 % 255 + 1
    return rows


class ScreenshotVisibilityTests(unittest.TestCase):
    def classify(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frame.png'
            path.write_bytes(data)
            return png_visibility.is_visible(path)

    def test_flat_white_and_red_survive_sub_and_up_filters(self):
        for color in (b'\xff\xff\xff', b'\xff\x00\x00'):
            with self.subTest(color=color):
                rows = [color * 320] * 240
                scanlines = filtered_rows(rows, 3, [1] + [2] * 239)
                self.assertTrue(self.classify(png_bytes(320, 240, 3, scanlines)))

    def test_black_rgb_and_opaque_rgba_ignore_alpha_and_filter_bytes(self):
        for channels in (3, 4):
            pixel = b'\x00\x00\x00' + (b'\xff' if channels == 4 else b'')
            rows = [pixel * 400] * 5
            for filtering in range(5):
                with self.subTest(channels=channels, filtering=filtering):
                    scanlines = filtered_rows(rows, channels, [filtering] * 5)
                    self.assertFalse(self.classify(
                        png_bytes(400, 5, channels, scanlines)))

    def test_all_filters_reconstruct_varying_adjacent_rows_at_threshold(self):
        for channels in (3, 4):
            for filtering in range(5):
                for nonzero in (999, 1000, 1001):
                    with self.subTest(channels=channels, filtering=filtering,
                                      nonzero=nonzero):
                        rows = sparse_rows(nonzero, channels)
                        scanlines = filtered_rows(rows, channels, [filtering] * 4)
                        self.assertEqual(self.classify(
                            png_bytes(400, 4, channels, scanlines)), nonzero >= 1000)

    def test_mixed_filters_preserve_previous_decoded_row(self):
        for nonzero in (999, 1000):
            with self.subTest(nonzero=nonzero):
                rows = sparse_rows(nonzero, 3, height=5)
                scanlines = filtered_rows(rows, 3, [0, 1, 2, 3, 4])
                self.assertEqual(self.classify(png_bytes(400, 5, 3, scanlines)),
                                 nonzero >= 1000)

    def test_paeth_ties_do_not_turn_zero_rgb_samples_into_visible_pixels(self):
        for left, above in ((40, 10), (10, 40)):
            with self.subTest(left=left, above=above):
                previous = bytearray(1002)
                previous[0] = 20
                previous[3] = above
                previous[6:] = b'\x01' * 996
                current = bytearray(1002)
                current[0] = left
                # Exactly 999 source samples are nonzero. At current[3],
                # Paeth ties left/corner or above/corner; both select 40,
                # giving residual 216 for this zero sample.
                encoded = bytearray(filtered_rows([previous, current], 3, [0, 4]))
                encoded[1003 + 1 + 3] = 216
                self.assertFalse(self.classify(png_bytes(334, 2, 3, encoded)))

    def test_invalid_or_unsupported_screenshots_raise_instead_of_classifying(self):
        valid = png_bytes(1, 1, 3, b'\x00\x00\x00\x00')
        damaged_crc = bytearray(valid)
        damaged_crc[29] ^= 1
        cases = {
            'signature': b'not a PNG',
            'truncated_chunk': valid[:-1],
            'checksum': damaged_crc,
            'missing_end': valid[:-12],
            'trailing_bytes': valid + b'x',
            'zero_width': png_bytes(0, 1, 3, b'\x00'),
            'sixteen_bit': png_bytes(1, 1, 3, b'\x00' * 7, depth=16),
            'grayscale': png_bytes(1, 1, 3, b'\x00\x00', color=0),
            'interlaced': png_bytes(1, 1, 3, b'\x00' * 4, interlace=1),
            'invalid_filter': png_bytes(1, 1, 3, b'\x05\x00\x00\x00'),
            'short_row': png_bytes(1, 1, 3, b'\x00' * 3),
            'extra_row_data': png_bytes(1, 1, 3, b'\x00' * 5),
            'invalid_zlib': png_bytes(1, 1, 3, b'', compressed=b'broken'),
            'truncated_zlib': png_bytes(1, 1, 3, b'',
                                       compressed=zlib.compress(b'\x00' * 4)[:-1]),
            'extra_zlib_stream': png_bytes(1, 1, 3, b'', compressed=(
                zlib.compress(b'\x00' * 4) + zlib.compress(b'\x00' * 4))),
        }
        for name, data in cases.items():
            with self.subTest(format=name):
                with self.assertRaises(ValueError):
                    self.classify(data)

    def test_visible_first_row_does_not_hide_later_invalid_scanlines(self):
        scanlines = b'\x00' + b'\xff' * 1200 + b'\x05' + b'\x00' * 1200
        with self.assertRaises(ValueError):
            self.classify(png_bytes(400, 2, 3, scanlines))

    def test_cli_distinguishes_visible_black_and_invalid(self):
        cases = (
            ('visible', png_bytes(400, 1, 3, b'\x00' + b'\xff' * 1200), 0),
            ('black', png_bytes(400, 1, 4, b'\x00' + b'\x00\x00\x00\xff' * 400), 1),
            ('invalid', b'not a PNG', 2),
        )
        with tempfile.TemporaryDirectory() as directory:
            for name, data, expected in cases:
                with self.subTest(frame=name):
                    path = Path(directory) / (name + '.png')
                    path.write_bytes(data)
                    result = subprocess.run([sys.executable, str(_HELPER), str(path)],
                                            capture_output=True, check=False)
                    self.assertEqual(result.returncode, expected)
                    if expected == 2:
                        self.assertIn(b'png_visibility:', result.stderr)
            missing = Path(directory) / 'missing.png'
            result = subprocess.run([sys.executable, str(_HELPER), str(missing)],
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'png_visibility:', result.stderr)


if __name__ == '__main__':
    unittest.main()
