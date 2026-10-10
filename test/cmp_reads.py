#!/usr/bin/env python3
"""opera-boot (GPL-2.0-only): CDCMD read-stream comparator for the matrix.

The candidate bootstrap is checked exactly against the disc's RomTags:
label, RTT, modern misc (or old-misc OS preload), splash, final OS load.
Retail's different bootstrap order is not compared as a subsequence.
Its boundary is the unique complete splash/misc/OS tail (OS alone for
old misc) before the first multi-block filesystem read. Missing, partial,
or ambiguous tails fail; no clean-room "boot" marker is needed.

All later READs expand to ordered physical-sector streams, compared as a
candidate subsequence of retail without sorting or deduplicating repeats.
Burst boundaries may differ; every sector requested still participates.
Inputs are complete CDCMD traces and 2048-byte ISOs with a valid first
label avatar, as required by read_rtt.py.

Usage: cmp_reads.py golden.log mine.log disc.iso
"""
import json
from pathlib import Path
import re
import subprocess
import sys

MARKER_RE = re.compile(r'@?CDCMD')
HEADER_RE = re.compile(r' (\d+) ')
COMMAND_RE = re.compile(r'[0-9a-fA-F]{2}(?: [0-9a-fA-F]{2}){6}')
RESUME_RE = re.compile(r'\n([0-9a-fA-F ]{1,19})\r?\n\Z')
HEADER_RESUME_RE = re.compile(
    r'\n( \d+ ' + COMMAND_RE.pattern + r')\r?\n\Z')


def command_bytes(record, path, seq):
    """Recover one stdout command interrupted by a contiguous kprint span."""
    m = COMMAND_RE.match(record)
    if m and record[m.end():].startswith(('\n', '\r\n')):
        return [int(x, 16) for x in m.group().split()]

    # fprintf emits exactly 20 characters plus newline. The resumed hex-only
    # final line fixes the split position, even if kprint starts with hex
    # ("3boot ...\n9 00 00 01" is sector 207, not a missing command).
    # Never invent bytes or use neighboring sector numbers to repair a trace.
    resume = RESUME_RE.search(record)
    if resume:
        suffix = resume.group(1)
        split = 20 - len(suffix)
        cmd = record[:split] + suffix
        inserted = record[split:resume.start() + 1]
        if ('\n' in inserted and re.search(r'[^0-9a-fA-F \r\n]', inserted)
                and COMMAND_RE.fullmatch(cmd)):
            return [int(x, 16) for x in cmd.split()]
    raise ValueError('%s: malformed/interleaved CDCMD %d' % (path, seq))


def reads(path):
    """Parse every command; retain READ (LBA, 24-bit block count) pairs."""
    with open(path, errors='replace') as log:
        text = log.read()
    markers = list(MARKER_RE.finditer(text))
    if not markers:
        raise ValueError('%s: no CDCMD trace' % path)
    out = []
    for i, marker in enumerate(markers):
        end = markers[i + 1].start() if i + 1 < len(markers) else len(text)
        record = text[marker.end():end]
        header = HEADER_RE.match(record)
        if header is None:
            # The same stdout/kprint insertion can occur immediately after
            # "CDCMD": the observed C&B trace resumes with the entire
            # " 664 10 27 1D 2F 00 00 05" line. Retain that actual record;
            # do not infer a missing sequence number from its neighbors.
            resume = HEADER_RESUME_RE.search(record)
            if resume and re.match(r'[^ \d\r\n]', record):
                record = resume.group(1) + '\n'
                header = HEADER_RE.match(record)
        if header is None:
            raise ValueError('%s: malformed/interleaved CDCMD header %d'
                             % (path, i))
        seq = int(header.group(1))
        if seq != i:
            raise ValueError('%s: missing/reordered CDCMD: expected %d, got %d'
                             % (path, i, seq))
        cmd = command_bytes(record[header.end():], path, seq)
        if cmd[0] == 0x10:
            block = cmd[1] * 4500 + cmd[2] * 75 + cmd[3]
            count = (cmd[4] << 16) | (cmd[5] << 8) | cmd[6]
            if cmd[2] >= 60 or cmd[3] >= 75 or not count:
                raise ValueError('%s: invalid READ CDCMD %d' % (path, seq))
            out.append((block, count))
    return out


def read_sectors(commands):
    """Stream sectors with their READ index/offset, without burst allocation."""
    for i, (block, count) in enumerate(commands):
        for offset in range(count):
            yield block + offset, i, offset


def unmatched_sector(small, big):
    """Return READ index, burst offset, and LBA of the first unmatched sector."""
    candidate = read_sectors(small)
    expected = next(candidate, None)
    for block, _, _ in read_sectors(big):
        if expected is None:
            return None
        if block == expected[0]:
            expected = next(candidate, None)
    return (expected[1], expected[2], expected[0]) if expected else None


def ceil2048(n):
    return (n + 2047) // 2048


def model_bootstrap(iso):
    """Match minidipir.c's first-tag selection and loader.c's sector rounding."""
    result = subprocess.run(
        [sys.executable, str(Path(__file__).with_name('read_rtt.py')),
         iso, '--json'], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip())
    info = json.loads(result.stdout)
    tags = []
    for tag in info['tags']:
        if tag['subsys'] == 0:             # disc_find_rt's terminator
            break
        tags.append(tag)

    def first_tag(want):
        return next((t for t in tags
                     if t['subsys'] == 0x0F and t['type'] == want), None)

    def sectors(tag):
        return [(b, 1) for b in range(
            tag['first_block'], tag['first_block'] + ceil2048(tag['size']))]

    os_tag = first_tag(0x07)
    misc_tag = first_tag(0x10)
    old_misc = misc_tag is None
    if old_misc:
        misc_tag = first_tag(0x0F)
    if os_tag is None or misc_tag is None or not os_tag['size']:
        raise ValueError('disc lacks required OS/misc bootstrap tags')
    if not old_misc and not misc_tag['size']:
        raise ValueError('disc has an empty modern misc component')
    splash_tag = first_tag(0x14)
    splash = sectors(splash_tag) if splash_tag and splash_tag['size'] else []
    os_reads = sectors(os_tag)
    misc = [] if old_misc else sectors(misc_tag)

    # Valid avatar 0 needs just one label probe and one RTT read. The source
    # stops probing immediately on success; do not add 375/33161 speculatively.
    # Old misc is rebuilt in memory, not read from its tag. minidipir still
    # stages the OS first, then reloads it after the optional splash.
    preload = os_reads if old_misc else misc
    model = [(150, 1), (151, 1)] + preload + splash + os_reads
    retail_tail = os_reads if old_misc else splash + misc + os_reads
    return model, retail_tail


def retail_boot_end(gold, tail):
    """Use complete component reads, not kprint timing, to locate handoff."""
    if gold[:2] != [(150, 1), (151, 1)]:
        raise ValueError('retail trace lacks initial label/RTT reads')
    burst = next((i for i, (_, count) in enumerate(gold) if count != 1), None)
    if burst is None:
        raise ValueError('retail trace is partial: no filesystem multi-block read')
    ends = [i + len(tail) for i in range(burst - len(tail) + 1)
            if gold[i:i + len(tail)] == tail]
    if len(ends) != 1:
        raise ValueError('retail bootstrap boundary is missing/partial or '
                         'ambiguous (%d complete component tails)' % len(ends))
    return ends[0]


def main():
    if len(sys.argv) != 4:
        print('usage: cmp_reads.py golden.log mine.log disc.iso', file=sys.stderr)
        return 1
    gold_path, mine_path, iso = sys.argv[1:]
    try:
        model, retail_tail = model_bootstrap(iso)
        gold = reads(gold_path)
        mine = reads(mine_path)
        for i, expected in enumerate(model):
            actual = mine[i] if i < len(mine) else None
            if actual != expected:
                raise ValueError('candidate bootstrap READ %d: expected %s, got %s'
                                 % (i, expected, actual))
        gold_end = retail_boot_end(gold, retail_tail)
        post = mine[len(model):]
        if not post:
            raise ValueError('candidate trace is partial: no post-bootstrap reads')
        missing = unmatched_sector(post, gold[gold_end:])
        if missing is not None:
            raise ValueError('candidate post-bootstrap READ %d offset %d LBA %d '
                             'is absent or reordered in retail '
                             '(physical-sector order and repeats retained)'
                             % missing)
    except (OSError, ValueError) as error:
        print('cdcmp: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
