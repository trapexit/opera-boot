#!/usr/bin/env python3
"""Classify 8-bit, noninterlaced RGB/RGBA harness PNG screenshots."""

import argparse
from pathlib import Path
import struct
import sys
import zlib


VISIBLE_BYTES = 1000
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'


def _read_png(data):
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError('invalid PNG signature')
    position = len(PNG_SIGNATURE)
    header = None
    compressed = bytearray()
    idat_seen = False
    idat_ended = False
    while position < len(data):
        if len(data) - position < 12:
            raise ValueError('truncated PNG chunk')
        length, kind = struct.unpack_from('>I4s', data, position)
        end = position + 12 + length
        if end > len(data):
            raise ValueError('truncated PNG chunk')
        payload = data[position + 8:end - 4]
        checksum = struct.unpack_from('>I', data, end - 4)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != checksum:
            raise ValueError('invalid PNG chunk checksum')
        if header is None and kind != b'IHDR':
            raise ValueError('PNG must start with IHDR')
        if kind == b'IHDR':
            if header is not None or length != 13:
                raise ValueError('invalid PNG header')
            header = struct.unpack('>IIBBBBB', payload)
            width, height, depth, color, compression, filtering, interlace = header
            if width == 0 or height == 0:
                raise ValueError('invalid PNG dimensions')
            if (depth != 8 or color not in (2, 6) or compression != 0
                    or filtering != 0 or interlace != 0):
                raise ValueError('unsupported PNG: expected 8-bit noninterlaced RGB/RGBA')
        elif kind == b'IDAT':
            if idat_ended:
                raise ValueError('nonconsecutive PNG image data')
            idat_seen = True
            compressed.extend(payload)
        elif kind == b'IEND':
            if length != 0 or not idat_seen or end != len(data):
                raise ValueError('invalid PNG end')
            return width, height, 3 if color == 2 else 4, compressed
        elif not kind[0] & 0x20:
            raise ValueError('unsupported PNG critical chunk')
        if idat_seen and kind != b'IDAT':
            idat_ended = True
        position = end
    raise ValueError('missing PNG end')


def _paeth(left, above, upper_left):
    prediction = left + above - upper_left
    left_distance = abs(prediction - left)
    above_distance = abs(prediction - above)
    corner_distance = abs(prediction - upper_left)
    if left_distance <= above_distance and left_distance <= corner_distance:
        return left
    if above_distance <= corner_distance:
        return above
    return upper_left


def is_visible(path) -> bool:
    """Count decoded nonzero RGB samples; raise ValueError for invalid PNGs.

    Alpha and scanline filter bytes do not count towards the 1000-byte threshold.
    File access errors propagate as OSError rather than being classified as black.
    """
    width, height, channels, compressed = _read_png(Path(path).read_bytes())
    stride = width * channels
    expected = height * (stride + 1)
    decoder = zlib.decompressobj()
    try:
        scanlines = decoder.decompress(compressed, expected + 1)
    except zlib.error as error:
        raise ValueError('invalid PNG compressed data') from error
    if (len(scanlines) != expected or not decoder.eof
            or decoder.unused_data or decoder.unconsumed_tail):
        raise ValueError('invalid PNG scanline data size or compressed stream')

    previous = bytearray(stride)
    nonzero = 0
    for row_number in range(height):
        offset = row_number * (stride + 1)
        filtering = scanlines[offset]
        if filtering > 4:
            raise ValueError('unsupported PNG scanline filter')
        row = bytearray(scanlines[offset + 1:offset + 1 + stride])
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filtering == 1:
                row[index] = (row[index] + left) & 255
            elif filtering == 2:
                row[index] = (row[index] + above) & 255
            elif filtering == 3:
                row[index] = (row[index] + (left + above) // 2) & 255
            elif filtering == 4:
                row[index] = (row[index] + _paeth(left, above, upper_left)) & 255
            if (channels == 3 or index % channels != 3) and row[index] != 0:
                nonzero += 1
        previous = row
    return nonzero >= VISIBLE_BYTES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    try:
        return 0 if is_visible(args.image) else 1
    except (OSError, ValueError) as error:
        print('png_visibility: %s' % error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
