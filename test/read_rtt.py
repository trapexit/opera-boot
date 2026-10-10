#!/usr/bin/env python3
"""opera-boot (GPL-2.0-only): dump a 3DO disc's volume label + RomTag table.

Reads block 150 (label) and 151 (RomTag table) from a 2048-byte-sector ISO
image and prints the tag list, plus the derived per-tag block ranges used by
the clean-room CDCMD comparator (strap-region filtering).

Usage: read_rtt.py disc.iso [--json]
"""
import json
import struct
import sys

SECTOR = 2048
LABEL_LBA = 150          # first avatar (dipir also probes +225 and +33011)
LABEL_FILE_BLOCK = 0     # these ISOs carry no lead-in: LBA n -> file n-150

TYPE_NAMES = {
    0x01: "RSA_MUST_RSA", 0x02: "BLOCKS_ALWAYS", 0x03: "BLOCKS_SOMETIMES",
    0x04: "BLOCKS_RANDOM", 0x05: "SIGNATURE_BLOCK", 0x06: "RSA_BOOT",
    0x07: "RSA_OS", 0x08: "CDINFO", 0x09: "NEWBOOT", 0x0A: "NEWNEWBOOT",
    0x0B: "NEWNEWGNUBOOT", 0x0C: "BILLSTUFF", 0x0D: "NEWKNEWNEWGNUBOOT",
    0x0F: "OLD_MISCCODE", 0x10: "MISCCODE", 0x11: "APP", 0x12: "DRIVER",
    0x13: "DEVDIPIR", 0x14: "APPSPLASH", 0x15: "DEPOTCONFIG",
    0x16: "DEVICE_INFO", 0x17: "DEV_PERMS", 0x18: "BOOT_OVERLAY",
}

STRAP_TYPES = {0x06, 0x09, 0x0A, 0x0B, 0x0D, 0x18}


def read_block(img, blk):
    img.seek(blk * SECTOR)
    data = img.read(SECTOR)
    if len(data) < SECTOR:
        raise SystemExit("short ISO: block %d unreadable" % blk)
    return data


def main():
    path = sys.argv[1]
    want_json = "--json" in sys.argv[2:]
    with open(path, "rb") as img:
        label = read_block(img, LABEL_FILE_BLOCK)
        if label[0] != 1 or label[1:6] != b"\x5a" * 5:
            raise SystemExit("no Opera volume label at file block %d (LBA %d)" % (LABEL_FILE_BLOCK, LABEL_LBA))
        ident = label[40:72].split(b"\0")[0].decode("ascii", "replace")
        volblocksize = struct.unpack(">I", label[76:80])[0]  # dl_VolumeBlockSize
        volblocks = struct.unpack(">I", label[80:84])[0]     # dl_VolumeBlockCount
        rtt = read_block(img, LABEL_FILE_BLOCK + 1)
        tags = []
        for off in range(0, SECTOR, 32):
            subsys, typ, ver, rev = rtt[off:off + 4]
            if subsys == 0 and typ == 0:
                break
            flags, tspec = rtt[off + 4], rtt[off + 5]
            toff, tsize = struct.unpack(">II", rtt[off + 8:off + 16])
            r3 = struct.unpack(">4I", rtt[off + 16:off + 32])
            t = {
                "subsys": subsys, "type": typ,
                "name": TYPE_NAMES.get(typ, "?%02x" % typ),
                "ver": ver, "rev": rev, "flags": flags, "tspec": tspec,
                "first_block": LABEL_LBA + 1 + toff,  # LBA
                "size": tsize,
                "blocks": (tsize + SECTOR - 1) // SECTOR,
                "r3": list(r3),
            }
            tags.append(t)

        strap = [t for t in tags if t["subsys"] == 0x0F
                 and t["type"] in STRAP_TYPES]

    out = {
        "ident": ident, "volblocks": volblocks,
        "volblocksize": volblocksize, "tags": tags,
        "strap_blocks": [[t["first_block"], t["first_block"] + t["blocks"]]
                         for t in strap],
    }
    if want_json:
        print(json.dumps(out, indent=2))
        return
    print('label "%s"  volblocks=%d  blocksize=%d' % (ident, volblocks, volblocksize))
    for t in tags:
        print("tag %02x/%-16s v%d.%d blk=%6d+%-3d size=%-8d r3=%s"
              % (t["subsys"], t["name"], t["ver"], t["rev"],
                 t["first_block"], t["blocks"], t["size"],
                 " ".join("%08x" % x for x in t["r3"])))
    for first, past in out["strap_blocks"]:
        print("strap region: blocks [%d,%d)" % (first, past))


if __name__ == "__main__":
    main()
