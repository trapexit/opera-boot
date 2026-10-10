#!/usr/bin/env python3
# opera-boot (GPL-2.0-only): dipir.bin map gate.
# Parses armlink -map output, computes the end of the last RO/RW/ZERO
# area plus the 2 KiB resident stack, and fails if it reaches the limit
# passed on argv[2].  The limit keeps the whole clean-room blob inside
# launch's zeroed low-DRAM region and below the component area.
import re
import sys

def main():
    mappath, limit = sys.argv[1], int(sys.argv[2], 0)
    end = 0
    line_re = re.compile(r'^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\w+)\s+' +
                          r'(RO|RW|ZERO)\s+(\S+) from object file (\S+)')
    for line in open(mappath):
        m = line_re.match(line.strip())
        if m:
            base, size, _typ = (int(m.group(1), 16), int(m.group(2), 16),
                                m.group(3))
            end = max(end, base + size)
    if end == 0:
        print("mapcheck: no areas found in map", file=sys.stderr)
        return 1
    need = end + 2048   # dipir.s stacksize
    print("mapcheck: image+stack end = 0x%X, limit 0x%X" % (need, limit))
    if need >= limit:
        print("mapcheck: FAIL (exceeds limit)", file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
