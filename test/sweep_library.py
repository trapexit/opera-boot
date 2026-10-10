#!/usr/bin/env python3
"""Run a sanitized opera-boot library sweep and emit a CSV manifest.

The CSV contains disc basenames and observed results only: no source paths,
usernames, output directories, or host identifiers. A boot is BIOS-pass only
when attempted primary/deep/control runs complete without execution errors,
the candidate handoff appears without a dipir failure, and no control-confirmed
BIOS rendering difference is found. Black boots receive a deep retry and an
optional retail-BIOS control. Execution errors and BIOS_DIFFERENCE set
bios_status=FAIL and make the command exit 1.
"""

import argparse
import concurrent.futures
import csv
import hashlib
import re
import subprocess
import sys
from datetime import date
import zlib
from collections import Counter
from pathlib import Path

from png_visibility import is_visible

BOOT_RE = re.compile(r"boot ([0-9A-Fa-f]{8})")
FAIL_RE = re.compile(
    r"(?:nolabel|not3do|noos|nodisc|nomisc|osread|osfit|miscfit|"
    r"miscread|badrec|badsize|noxbus|dabt|"
    r"(?:^|[\r\n])opera-boot: TRAP (?:undf|pabt|dabt|addr) lr=[0-9A-Fa-f]{8})"
)
SECTOR_BYTES = 2048

UNBOUNDED_SENSITIVE = {
    "Twisted (Japan)",
    "Twisted - The Game Show (Europe)",
    "Twisted - The Game Show (USA)",
    "Twisted - The Game Show (USA) (Alt)",
    "Twisted - The Game Show (USA) (Beta) (1994-03-21)",
    "Olympic Soccer (USA) (En,Fr,De,Es,It)",
    "Olympic Summer Games (USA)",
    "Seal of the Pharaoh (USA)",
    "3DO Interactive Sampler CD, The (USA)",
    "Virtual Quest - Pharaoh no Fuuin (Japan)",
}


def parse_args():
    root = Path(__file__).resolve().parents[1]
    opera = root.parent / "opera-libretro"
    parser = argparse.ArgumentParser()
    parser.add_argument("--bios", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runs-dir", required=True, type=Path)
    parser.add_argument("--core", type=Path, default=opera / "opera_libretro.so")
    parser.add_argument("--harness", type=Path, default=opera / "opera-test-harness")
    parser.add_argument("--control-bios", type=Path)
    parser.add_argument("--font", type=Path)
    parser.add_argument("--frames", type=int, default=3600)
    parser.add_argument("--deep-frames", type=int, default=5400)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--cd-speed", default="2x")
    return parser.parse_args()


def disc_version(path):
    with path.open("rb") as image:
        label = image.read(SECTOR_BYTES)
        if len(label) != SECTOR_BYTES or label[0] != 1 or label[1:6] != b"\x5a" * 5:
            return "unknown"
        table = image.read(SECTOR_BYTES)
    for offset in range(0, SECTOR_BYTES, 32):
        subsys, kind, version, revision = table[offset:offset + 4]
        if subsys == 0:
            break
        if subsys == 0x0F and kind == 0x07:
            return "%d.%d" % (version, revision)
    return "unknown"


def safe_name(path):
    digest = hashlib.sha1(path.name.encode("utf-8")).hexdigest()[:8]
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", path.stem).strip("_")[:100]
    return "%s_%s" % (stem, digest)


def run_harness(args, iso, bios, output, frames):
    output.mkdir(parents=True, exist_ok=True)
    shots = sorted(set(range(900, frames + 1, 900)) | {frames})
    command = [
        str(args.harness),
        "--core", str(args.core),
        "--bios", str(bios),
        "--title", str(iso),
        "--frames", str(frames),
        "--option", "opera_kprint=enabled",
        "--option", "opera_random_seed=1",
        "--option", "opera_cd_speed=%s" % args.cd_speed,
        "--output-dir", str(output),
        "--wall-timeout", "180",
    ]
    if args.font is not None:
        command.extend(["--font", str(args.font)])
    for frame in shots:
        command.extend([
            "--screenshot-at",
            "%d=%s" % (frame, output / ("f%d.png" % frame)),
        ])
    try:
        result = subprocess.run(
            command,
            cwd=args.harness.parent,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=240,
            check=False,
        )
        rc = result.returncode
    except subprocess.TimeoutExpired:
        rc = 124
    log_path = output / "run.log"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    marker = BOOT_RE.search(log)
    visible_frame = ""
    for frame in shots:
        shot = output / ("f%d.png" % frame)
        if shot.exists() and is_visible(shot):
            visible_frame = str(frame)
            break
    return {
        "rc": rc,
        "boot_address": marker.group(1).lower() if marker else "",
        "failed": bool(FAIL_RE.search(log)),
        "visible_frame": visible_frame,
    }


def sweep_one(args, iso):
    name = iso.stem
    run_root = args.runs_dir / safe_name(iso)
    primary = run_harness(args, iso, args.bios, run_root / "bios", args.frames)
    observed = primary
    execution_note = ""
    primary_ok = primary["rc"] == 0 and not primary["failed"]
    if not primary_ok:
        render_status = "PRIMARY_ERROR"
        execution_note = "primary execution error (rc=%d%s)" % (
            primary["rc"], ", fatal marker" if primary["failed"] else ""
        )
    else:
        render_status = "PASS" if primary["visible_frame"] else ""

    if primary_ok and primary["boot_address"] and not render_status:
        observed = run_harness(
            args, iso, args.bios, run_root / "bios-deep", args.deep_frames
        )
        if observed["rc"] != 0 or observed["failed"]:
            render_status = "DEEP_ERROR"
            execution_note = "deep execution error (rc=%d%s)" % (
                observed["rc"], ", fatal marker" if observed["failed"] else ""
            )
        elif observed["visible_frame"]:
            render_status = "PASS_DEEP"

    control_status = ""
    if primary_ok and primary["boot_address"] and not render_status:
        render_status = "BLACK_UNCONTROLLED"
        if args.control_bios:
            control = run_harness(
                args,
                iso,
                args.control_bios,
                run_root / "control-deep",
                args.deep_frames,
            )
            if control["rc"] != 0 or control["failed"]:
                control_status = "ERROR"
                render_status = "CONTROL_ERROR"
                execution_note = "control execution error (rc=%d%s)" % (
                    control["rc"], ", fatal marker" if control["failed"] else ""
                )
            elif control["visible_frame"]:
                control_status = "VISIBLE"
                render_status = "BIOS_DIFFERENCE"
            else:
                control_status = "BLACK"
                render_status = "BLACK_CORE_OR_DUMP"

    bios_status = "PASS"
    if (primary["rc"] != 0 or not primary["boot_address"] or primary["failed"]
            or render_status in ("DEEP_ERROR", "CONTROL_ERROR", "BIOS_DIFFERENCE")):
        bios_status = "FAIL"
        if not render_status:
            render_status = "NOT_REACHED"

    notes = ""
    if name in UNBOUNDED_SENSITIVE:
        notes = "black at unbounded speed; renders with bounded CD speed"
    elif name == "Real Pinball (Europe)":
        notes = "slow first frame; speed-independent"
    if execution_note:
        notes = execution_note

    return {
        "sweep_date": args.sweep_date,
        "bios_crc32": args.bios_crc32,
        "title": name,
        "os_version": disc_version(iso),
        "boot_address": primary["boot_address"],
        "bios_status": bios_status,
        "render_status": render_status,
        "cd_speed": args.cd_speed,
        "first_visible_frame": observed["visible_frame"],
        "control_status": control_status,
        "notes": notes,
    }


def main():
    args = parse_args()
    args.library = args.library.resolve()
    images = sorted(args.library.glob("*.iso"), key=lambda path: path.name.casefold())
    if not images:
        print("sweep_library: no ISO images found", file=sys.stderr)
        return 2
    for path in (args.bios, args.core, args.harness):
        if not path.is_file():
            print("sweep_library: missing %s" % path, file=sys.stderr)
            return 2
    if args.control_bios and not args.control_bios.is_file():
        print("sweep_library: missing control BIOS %s" % args.control_bios,
              file=sys.stderr)
        return 2
    if args.font is not None and not args.font.is_file():
        print("sweep_library: missing font %s" % args.font, file=sys.stderr)
        return 2

    args.bios = args.bios.resolve()
    args.output = args.output.resolve()
    args.runs_dir = args.runs_dir.resolve()
    args.core = args.core.resolve()
    args.harness = args.harness.resolve()
    if args.control_bios:
        args.control_bios = args.control_bios.resolve()
    if args.font is not None:
        args.font = args.font.resolve()
    args.sweep_date = date.today().isoformat()
    args.bios_crc32 = "%08x" % (zlib.crc32(args.bios.read_bytes()) & 0xFFFFFFFF)
    args.runs_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        future_to_image = {
            pool.submit(sweep_one, args, image): image for image in images
        }
        for completed, future in enumerate(
            concurrent.futures.as_completed(future_to_image), 1
        ):
            image = future_to_image[future]
            try:
                row = future.result()
            except Exception as error:
                row = {
                    "sweep_date": args.sweep_date,
                    "bios_crc32": args.bios_crc32,
                    "title": image.stem,
                    "os_version": disc_version(image),
                    "boot_address": "",
                    "bios_status": "FAIL",
                    "render_status": "HARNESS_ERROR",
                    "cd_speed": args.cd_speed,
                    "first_visible_frame": "",
                    "control_status": "",
                    "notes": "runner exception: %s" % type(error).__name__,
                }
            rows.append(row)
            print("[%d/%d] %s: %s / %s" % (
                completed,
                len(images),
                row["title"],
                row["bios_status"],
                row["render_status"],
            ), flush=True)

    rows.sort(key=lambda row: row["title"].casefold())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    fields = [
        "sweep_date",
        "bios_crc32",
        "title",
        "os_version",
        "boot_address",
        "bios_status",
        "render_status",
        "cd_speed",
        "first_visible_frame",
        "control_status",
        "notes",
    ]
    with temporary.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    temporary.replace(args.output)

    counts = Counter(row["render_status"] for row in rows)
    print("sweep_library: %d discs; %s" % (
        len(rows),
        ", ".join("%s=%d" % item for item in sorted(counts.items())),
    ))
    return 1 if any(row["bios_status"] != "PASS" for row in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
