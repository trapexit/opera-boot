# opera-boot technical reference

opera-boot is a minimal open-source alternative to retail 3DO boot ROMs.
It boots the operating system supplied by the CD rather than providing an
operating system of its own. The target is an ARM60, big-endian,
FZ-1-class machine and the Opera libretro core. The hardware addresses
below describe that target; they do not establish real-hardware compatibility.

Bootstrap code is licensed under [GNU GPL version 2 only](../LICENSE)
(`GPL-2.0-only`). The implementation
is clean-room code, not copied from Portfolio or a retail ROM. The startup
logo and wordmark are third-party artwork and are not covered by that code
license.

## Execution and image layout

The runtime targets ARMv3 and uses no C library. Its assembly uses the ARM
SDT 2.51 toolchain dialect (`armcc` 4.91, `armasm` 2.50, `armlink` 5.20).
Components are linked directly with `armlink -bin -ro-base <address>`,
including zero-initialized storage. The data-only RomTag table uses its base
as linker entry metadata; the flat binary has no executable header.
The dipir blob is linked for DRAM address `0x200`.
The image is a zero-filled 128 KiB ROM:

| ROM offset | Contents |
| --- | --- |
| `0x00000` | Launch/reset code and startup banner |
| `0x01000` | ROM RomTag table, subsystem `0x10` |
| `0x02000` | Resident dipir blob, copied to DRAM `0x200` |
| `0x1F000` | 256 zero bytes used as the initial current-OS header |

The ROM's permanent mapping is `0x03000000`, but the CPU resets at address
zero through a temporary low-memory ROM alias. ROM word 0 is
`ldr pc,[pc,#-4]`; word 1 contains `0x03000008`, the high-ROM `launch_start`.
That entry selects SVC mode with IRQ/FIQ masked (`CPSR=0xD3`) and writes zero
to DRAM `0x3C`, releasing the overlay before reading persistent boot cells.
Resident dipir code, data, zero-initialized storage, and its 2 KiB private
stack occupy low DRAM below `0x8000`; executable disc components start at
`0x10000`.

## Memory and hardware registers

Stock DRAM is 2 MiB at `0x00000000`. DRAM size is decoded from MADAM MSYS
at `0x03300004`. For register value `v`, the size in bytes is:

```text
b0 = 1 << ((v >> 4) & 6)
b1 = 1 << ((v >> 2) & 6)
dram_size = ((b0 >> 2) + (b1 >> 2)) << 20
```

The 1 MiB VRAM aperture follows DRAM. The startup framebuffer is at
`dram_size + 0xC0000` (`0x2C0000` on a stock machine), and the startup VDL
is at `dram_size + 0xB0000`.

| Register | Address or offset | Use |
| --- | --- | --- |
| MADAM | `0x03300000` | Memory/video controller base |
| MCTL | MADAM `+0x08` | `VSCTXEN=0x4000`, `CLUTXEN=0x2000` |
| Abort reason | MADAM `+0x20` | Read, then clear by writing zero |
| VDL head | MADAM `+0x580` | Combined-aperture VDL address |
| CLIO | `0x03400000` | I/O controller base |
| CSTAT | CLIO `+0x28` | Reset reason; launch writes `0x20` to clear the reset-reason bit |
| VCNT | CLIO `+0x34` | Scanline counter, masked with `0x7FF` |
| Interrupt clear | CLIO `+0x44`, `+0x64` | Clear pending interrupts |
| Interrupt enable clear | CLIO `+0x4C`, `+0x6C` | Clear interrupt enables |
| ADBIO | CLIO `+0x84` | ROM selection and OTHERROM enable |
| TIM14 count/countback | CLIO `+0x170`, `+0x174` | Both initialized to 61 |
| Timer set/clear | CLIO `+0x208`, `+0x20C` | Clear `0xFF000000`, then set `0x73000000` |
| Timer slack | CLIO `+0x220` | Saved, set to 336 during full initialization, restored on exit |
| XBUS | `0x03400400` | Expansion bus base |

The timer configuration cascades timers 14 and 15 at approximately 1 ms.
Scanline-based delays use approximately 15.75 line transitions per ms.
The MCTL enable sequence uses the safe window `10 <= (VCNT & 0x7FF) < 13`:
VSCTXEN is enabled first, then the next frame's safe window enables CLUTXEN.
VCNT-based delays and video-window waits have no stopped-counter timeout;
only timing classification and XBUS polling have explicit iteration limits.

Interrupt offsets `+0x48` and `+0x68` set enables; they are not disable
registers. Handoff clears enables through `+0x4C`/`+0x6C` and clears pending
bits through `+0x44`/`+0x64`.

Full initialization saves ADBIO and writes `(saved | 0x40) & ~0x04` to enable
OTHERROM while selecting the primary ROM. Exit writes `saved | 0x40`, leaving
OTHERROM enabled, and restores timer slack; timer count/control registers
are not restored. A 32-bit store containing a character to MADAM `+0x00`
supplies Opera's kprint channel.

## Launch and persistent boot cells

| DRAM address | Meaning |
| --- | --- |
| `0x28` | Current kernel AIF pointer; initially `0x0301F000` |
| `0x2C` | ROM RomTag table address, `0x03001000` |
| `0x30` | Warm-reset entry (`launch_start`) |
| `0x34` | Sentinel `0xB00710AD` |
| `0x38` | Cached timing family: 0 NTSC, 1 PAL |
| `0x3C` | Overlay-release scratch word, zeroed on each launcher entry |
| `0x40` | Pending resident DIPIR-reset continuation, normally zero |

Cold launch reports and clears the reset reason, zeroes DRAM
`0x0000..0xFFFF`, installs exception vectors, copies the word-padded dipir
blob to `0x200`, and initializes the boot cells. It does not clear VRAM.
Initial reset, undefined, prefetch-abort, data-abort, and address-exception
vectors self-loop; SWI uses `movs pc,lr`; IRQ/FIQ use `sub pc,lr,#4`
(`0xE24EF004`, without SPSR restoration).

Timing detection reads only masked VCNT. A scanline of 264 or later selects
PAL; otherwise two counter wraps select NTSC. The sample limit is
`0x100000`, after which the default is NTSC. Both Opera PAL width modes
share PAL timing. This probe is stackless and changes no timers or video
registers. Warm reset and runtime dipir calls retain the cached result.

The final cold-launch SVC stack is at `dram_size - 16`. Banked stack tops
are FIQ at `dram_size - 0x10000`, IRQ at `dram_size - 0x11000`, ABT at
`dram_size - 0x13000`, and UND at `dram_size - 0x14000`. Launch leaves
hardware DIPIR latches intact, paints the startup banner once, and calls
dipir at `0x200`. Returned attempts are retried after 3150 scanline
transitions (approximately 200 ms), without repainting the banner.

A pending resident DIPIR reset takes precedence over a warm kernel jump.
With the sentinel valid, CSTAT reset-reason bit `0x40` set, and a nonzero
continuation at `[0x40]`, launch executes `movs pc,r2` to that continuation,
restoring the interrupted CPSR from SPSR_svc. The resident helper clears
`[0x40]` and restores the saved ADBIO, `r0..r5`, `r12`, and return address
from the original stack. It resumes the interrupted invocation without
clearing DRAM, changing stack tops, repainting, or entering the staged kernel.

A warm reset with the sentinel present and a non-placeholder kernel in
`[0x28]` resets only the SVC stack and jumps to that kernel, preserving the
caller's `r6`, `r7`, and `r8`. Otherwise launch takes the cold path.

## Kernel-facing dipir service table

The table at DRAM `0x200` has the following 11-word layout. All service
words except the count are ARM branch instructions.

| Offset | Service | Result or behavior |
| --- | --- | --- |
| `+0x00` | dipir entry | Cold or runtime boot attempt |
| `+0x04` | RSACheck | 1; no signature verification |
| `+0x08` | GenRSACheck | 1; no signature verification |
| `+0x0C` | QueryROMSysInfo | Scalar timing queries described below |
| `+0x10` | SetROMSysInfo | `0xFFFFFFFE` (unsupported) |
| `+0x14` | SectorECC | 0 |
| `+0x18` | Entry count | Literal 11 |
| `+0x1C` | OpenRomFile | NULL |
| `+0x20` | ReadRomFile | 0 |
| `+0x24` | CloseRomFile | 0 |
| `+0x28` | Disc OS version | `(version << 8) | revision` from the kernel at `[0x28]` |

The version and revision are the `b3_nVersion`/`b3_nRevision` bytes at
offsets 20/21 of the 3DO binary header, which follows the 128-byte AIF header.
The dipir entry saves the caller's SP/LR and switches to its private stack.

`QueryROMSysInfo` uses the ARM ABI for
`u32 (u32 tag, void *info, u32 info_size)`: inputs in `r0..r2`, scalar
result in `r0`, and normal callee-saved registers preserved.

| Tag | NTSC (`[0x38]=0`) | PAL (`[0x38]=1`) |
| --- | --- | --- |
| `SYSINFO_TAG_FIELDFREQ` (`0x10001`) | 60 | 50 |
| `SYSINFO_TAG_GRAPHDISPSUPP` (`0x20004`) | `0x00010101` | `0x00020202` |

Display values combine supported, default, and current timing-family bits.
The queries do not write `info`, and NULL/zero size is valid. Other tags or
unrecognized cached modes return `0xFFFFFFFE`. All SetROMSysInfo calls are
unsupported. There are no geometry queries or timing-switch operations.

## Full boot and runtime re-entry

1. Attach to MEI CD unit 0 and flush stale status. If a kernel is already
   published, await the drive and probe labels using the saved booted disc's
   TOC-derived first block. Matching volume identifier and block count return
   immediately, without video, timer, ADBIO, or startup-VDL initialization.
   Disc unique identifiers are not used. The probe uses resident scratch
   storage rather than modifying the label published to the kernel or the
   running title's upper DRAM.
2. On a full attempt, enable video, initialize timers/ADBIO, and install the
   startup VDL. Poll device status up to 80 times with 787-line delays
   (approximately 50 ms). An open door, missing disc, command failure, or
   exhaustion returns. Once ready, send spin-up if the spinning bit is absent.
3. Check disc shape: DiscId 0, first track 1, last track at least 1, no
   multisession flag (`0x80`), and track 1 marked digital data (`AddrCntrl & 4`).
   Audio-preemphasis (`1`) and four-channel (`8`) flags are rejected.
4. Convert track 1's TOC MSF to absolute frames, probe the label, and read
   its following RomTag block. The table needs an in-sector terminator and
   at least two tags. An OS tag and a modern or old misc tag are required.
   Device permissions come from `RSA_DEV_PERMS.rt_Reserved3[0]` as a
   32-bit value, defaulting to zero.
5. Load and relocate modern misc. For `RSA_OLD_MISCCODE`, preload the OS
   image and publish the resident old-misc replacement instead. Then load
   and display an optional app splash. A failed or invalid optional splash
   does not prevent the final OS load.
6. Reload the OS into the staging area and relocate its executable list.
   The first executable in this list is the kernel. Clear inter-component
   gaps, restore ADBIO/slack, save generic disc identity, and hand off using
   the kernel-version-dependent ABI below.

The disc OS always runs; there is no ROM-versus-disc OS version race.
NEWBOOT, BOOT_OVERLAY, and NEWKNEWNEWGNUBOOT tags are not downloaded or
executed.

## Disc formats and addressing

Disc sectors contain 2048 bytes. MEI addresses are absolute MSF frames:
`frame + 75 * second + 4500 * minute`. There is no subtraction of the
150-frame lead-in in this protocol. Label probes use the TOC-derived first
block plus offsets `0`, `225`, `0`, and `33011`, in that order. For a first
block of 150 these are 150, 375, 150, and 33161.

The 132-byte big-endian DiscLabel layout is:

| Byte offset | Field |
| --- | --- |
| 0 | Record type (1) |
| 1..5 | Five sync bytes (`0x5A`) |
| 6, 7 | Structure version, volume flags |
| 8..39 | Commentary |
| 40..71 | Volume identifier |
| 72 | Volume unique identifier (`u32`) |
| 76, 80 | Volume block size/count (`u32` each) |
| 84 | Root unique identifier (`u32`) |
| 88, 92 | Root directory block count/size (`u32` each) |
| 96 | Last root avatar index (`u32`) |
| 100..131 | Eight root directory avatar addresses (`u32` each) |

Label validation checks record type and sync bytes only. The RomTag table
occupies the next sector and contains 32-byte entries: eight bytes for
subsystem, type, version, revision, flags, type-specific, and two reserved
fields; `u32 offset`, `u32 size`, and four reserved `u32` words. A zero
subsystem terminates the table. Offsets are sectors relative to the RomTag
block. Sizes include the trailing 64-byte signature, which is not checked.

Relevant disc tags use subsystem `0x0F`: OS `0x07`, old misc `0x0F`, modern
misc `0x10`, app splash `0x14`, and device permissions `0x17`.

## Component relocation and bounds

Components stage at DRAM `0xA0000`, with sector-rounded reads. Zero size,
original size beyond `dram_size - 0xA0000`, or a rounded span beyond that
capacity is rejected before reading. The original-size guard precedes
rounding so `size + 2047` cannot wrap into an accepted read.

Each modern misc/OS image has an 8-byte prelude followed by word-aligned
records: a destination address, a byte size, then executable bytes. A zero
address terminates the list. Every address/size header, payload, and
terminator is bounded by declared `rt_Size`, not sector padding. A list
must contain at least one executable of its own; trailing signature or
padding after an in-bounds terminator is accepted. At most eight components
are retained, including the old-misc replacement when present.

Each record is at least 128 bytes, has a destination in
`[0x10000, dram_size)`, fits in DRAM, and does not overlap the sector-rounded
staging span. The mastered destination address is authoritative; no extra
kernel stack reserve is added. After each executable, the running pointer
is set to destination plus AIF RO+RW+ZI sizes (header words 5, 6, and 8).
It is rounded to 1 KiB before OS relocation; record addresses still determine
actual placement. Consecutive non-overlapping component gaps are zeroed
before handoff.

Word alignment is a format assumption, not an explicit record validation.
The loader does not validate AIF live-size sums, reject overlaps between
separate relocated components, or require a complete 3DO header beyond the
128-byte AIF minimum. Copies operate in whole words.

These are loader bounds, not signature authentication. RSA services return
success unconditionally; this ROM does not authenticate disc content.

## Old-misc ABI

The old-misc replacement occupies a 3 KiB record at DRAM `0x0C000`, below
the executable destination floor. It does not advance the component load
pointer. Its first two words are `MOV r0,pc` (`0xE1A0000F`) and `MOV pc,r8`
(`0xE1A0F008`), followed by 14 absolute routine pointers:

| Slot | Routine |
| --- | --- |
| 0, 1 | memcpy, memset |
| 2, 3 | RemNode, InsertNodeFromTail |
| 4, 5 | AddTail, RemTail |
| 6, 7 | AddHead, RemHead |
| 8, 9 | InsertNodeFromHead, InitList |
| 10, 11 | Signed divide, unsigned divide |
| 12, 13 | Signed divide by ten, unsigned divide by ten |

Routine bodies remain in the resident low-DRAM blob. The disc's
self-expanding misc unpacker is not executed in dipir context. Copy/fill
routines use byte access for unaligned inputs. List operations use the
Portfolio-compatible node/list field layout and sentinel anchors.
General divide takes divisor first and dividend second, returning quotient
in `r0` and remainder in `r1`. Signed division truncates toward zero and
gives the remainder the dividend's sign. Division by zero returns quotient
zero and the original dividend as remainder.

Node/list fields use big-endian values and 32-bit pointers:

| Node byte offset | Field |
| --- | --- |
| 0, 4 | Next and previous node pointers |
| 8, 9 | Subsystem and type (`u8` each) |
| 10, 11 | Priority and flags (`u8` each) |
| 12 | Size (`s32`) |
| 16 | Name pointer |

A node is 20 bytes. A list is 32 bytes: its identity node occupies bytes
0..19, followed by first-node pointer at 20, tail marker at 24, and last-node
pointer at 28. The tail marker is NULL. In an empty list, the first pointer
is the address of the tail-marker field and the last pointer is the address
of the first-pointer field. The first real node's previous link and the
last real node's next link point at these head and tail anchors.
InitList sets subsystem 1, type 2, priority 0, flags `0x80`, size 32,
the supplied name, and NULL identity-node next/previous links. Sorted
insertion orders nodes by descending priority; head insertion precedes equal
priorities, while tail insertion follows them. RemHead/RemTail return NULL
for an empty list.

## Display and splash format

The startup display is fixed at 320x240, 16 bits per pixel, using MADAM's
LRFORM line-pair interleave. Pixel `(x,y)` has `u16` index
`(y >> 1) * 640 + x * 2 + (y & 1)` from the framebuffer base.

The 46-word VDL begins at VRAM offset `0xB0000`:

```text
word 0:      0x00004410
words 1, 2:  framebuffer
word 3:      dram_size + 0xB0098
words 4..35: grey CLUT entries (i << 24) | (v << 16) | (v << 8) | v
             for i = 0..31, with v = round(i * 255 / 31),
             except i = 30 uses v = 248 (word 34 = 0x1EF8F8F8)
words 36..45:
  0xE0010101, 0xC001002C, 0x002180EF, framebuffer, framebuffer,
  dram_size + 0xB00A8, 0, framebuffer, framebuffer, dram_size + 0xB0000
```

Only pointer words are adjusted for DRAM size. Same-disc runtime re-entry
does not replace the live OS's VDL or write its VRAM.

The startup banner is a black background with the four-color 3DO totem,
white `3DO` wordmark, and a small `Opera Boot (YYYY-MM-DD)` caption. The date
is generated at build time from the current UTC date, or from Make's
`BUILD_DATE` override for reproducible rebuilds. Original 5x7 caption glyphs
are packed MSB-first with one-pixel spacing and trailing unused columns
trimmed for horizontal ink centering; the caption starts at row 194.
The emblem span data derives from
`3do-devkit/art/Small LPS/Small 4 Custom Color HR_on Blk`; this is third-party
artwork, not original project art. The framebuffer width is not changed
for PAL2, where the banner can be garbled.

An app splash has a 24-byte VideoImage header: version byte; seven-byte
`APPSCRN` pattern; `u32 size`; `u16 height`, `u16 width`; depth, type, two
reserved bytes; and a reserved `u32`. Supported depths are 1, 2, 4, 8, and
16. Packed fields are read most-significant-first and rows advance in
interleaved pairs. The framebuffer is black-filled before header validation.

The loaded `rt_Size`, not the header's size metadata, bounds the image.
Validation requires a complete aligned header, matching pattern, nonzero
dimensions, and supported depth. For `L = width * depth`, let
`line_bytes = floor(L/8)` and `pair_bytes = ceil(L/4)`. Required payload is
`(ceil(height/2)-1) * 2 * line_bytes + pair_bytes`; an odd height still
requires a complete final pair. The final-pair half-offset is compared to
`floor((payload_bytes - pair_bytes)/2)` only after `pair_bytes` fits, avoiding
an overflow in doubling. Pixel and cropping pointers are formed afterward.
Images are centered and cropped to the display; vertical positioning is
line-pair aligned. Horizontal cropping is selected from the actual image
width, not its rounded margin: a 321-pixel-wide image still draws only 320
columns per pair. Cropping retains the full source-image stride. Pixel values
are scaled by `floor(65536/(2^depth-1))` and clamped to `0xFFFF`.
Rejected optional artwork is ignored.

## MEI transport

Unit 0 is selected by writing zero to XBUS SEL. Attach waits for CPUHASXBUS
(`0x80`) in SETEXPCTL, reads POLL, and acknowledges MEDIA_ACCESS with a
write-one-to-clear POLL bit `0x80`. That acknowledgement can set a CURRENT
DIPIR latch (`0x8000`), which protects CMDSTAT and DATA access. If either
DIPIR latch is CURRENT, the ROM saves its caller frame and ADBIO, clears
ADBIO ROM2-select bit `0x04` to select the boot ROM, publishes its continuation
at DRAM `0x40`, and writes CSTAT `0x30` (`SOFTRES | CLRDIP`).
The CPU/controller reset converts CURRENT to before-reset bit `0x4000`
while preserving the acknowledged external drive. Launch resumes the
resident helper, which consumes the continuation and restores the caller.
Only then does attachment drain stale status or issue commands. No synthetic
DIPIR latch writes or external-drive reset are used for this acknowledgement.

| XBUS register | Offset |
| --- | --- |
| SETEXPCTL / CLREXPCTL | `+0x000` / `+0x004` |
| DIPIR1 / DIPIR2 | `+0x010` / `+0x014` |
| SEL / POLL | `+0x100` / `+0x140` |
| CMDSTAT / DATA | `+0x180` / `+0x1C0` |

POLL bits are status/read/write interrupt enable `0x01`/`0x02`/`0x04`,
RESET `0x08`, status/read/write valid `0x10`/`0x20`/`0x40`, and
MEDIA_ACCESS `0x80`. Commands write seven bytes to CMDSTAT. Replies are
collected while STAT_VALID is set; the status buffer holds 16 bytes and
oversized replies are rejected. The final status byte carries error bit
`0x10`. Reads register a 2048-byte sink before issuing the command, then
burst-drain the pending data when READ_VALID appears, without per-byte
polling. Command waits are bounded to 4,000,000 poll iterations.

| Command | Code | Command encoding / reply |
| --- | --- | --- |
| Device status | `0x82` | Zero-filled remaining bytes |
| Spin up | `0x02` | Zero-filled remaining bytes |
| Disc info | `0x8B` | Remaining command bytes zero; seven-byte info reply |
| Session info | `0x8D` | Remaining command bytes zero; reply flags contain multisession bit |
| Track TOC | `0x8C` | Track number in command byte 2 |
| Read one block | `0x10` | Bytes 1..3 are minute/second/frame; byte 6 is 1 |

Disc-info fields occupy reply bytes 0..6: command echo, DiscId, first
track, last track, and end-address minute/second/frame. Session flags are
read from reply byte 1. Track-TOC fields occupy bytes 0..8: command echo,
reserved, AddrCntrl, track number, reserved, track-start minute/second/frame,
and reserved. MSF components are binary bytes, not BCD.

## Abort recovery and kernel handoff

Each attempt temporarily replaces data-abort vector `0x10`, retaining its
previous word for restoration on return to a live kernel. The recovery
buffer saves `r4..r11`, SP, and LR; its pointer is published only after the
save. The stackless handler reads/clears MADAM's abort reason, changes
ABT to SVC while retaining interrupt masks, and restores the saved context.
A caught abort reports `dabt`, asserts XBUS POLL RESET for 1575 scanline
transitions (about 100 ms), releases it for 9450 transitions (about 600 ms),
restores saved slack, ADBIO selection, and the previous vector, and returns.
There are no other drive commands during that recovery sequence. An unarmed
abort is fatal. Other failed boot attempts also return after cleanup; they
are not fatal traps.

Before kernel entry, undefined, address, prefetch-abort, and data-abort
vectors become fault-reporting traps. The recovery net is disarmed without
restoring its saved vector over those traps. CLIO interrupt enables and
pending bits are cleared, and `[0x28]` receives the kernel AIF address.
The final jump uses:

| Kernel `b3_nVersion` | `r6` | `r7` |
| --- | --- | --- |
| Less than 24 | `0x100` | Pointer to the resident DiscLabel |
| 24 or greater | `0x101` | Pointer to BootData |

BootData consists of three words: version 1, pointer to the resident label,
and device permissions. In both cases `r8` is the first component's address
(modern misc or the old-misc replacement), and execution jumps to the start
of the kernel AIF image, not a separately decoded entrypoint. The loader
does not itself execute the AIF decompression, self-relocation, or
zero-initialization helpers.

Boot-attempt diagnostics use the MADAM kprint channel. Every textual record
starts with `opera-boot: `, exactly once, including errors and fatal traps.
Failed attempts report short identifiers such as `opera-boot: nodisc`,
`opera-boot: nolabel`, `opera-boot: noos`, or `opera-boot: osfit`.
The early raw reset-reason byte is a hardware value, not a textual diagnostic;
it remains unchanged, as do emulator, disc OS, and application messages.

Successful component loading emits disc identity/geometry and physical label
locations, selected RomTag metadata, effective permissions, loaded component
addresses/sizes, kernel AIF sizes, kernel version/revision, and boot-argument ABI
before the final `boot` marker. Only selected tags are reported; their versions
do not affect selection. The kernel version comes from its own 3DO header, not
the OS tag. Old-misc reconstruction is explicitly identified as resident code.
Optional absent tags are marked missing. Additional AIF/3DO diagnostic reads
are bounded by the recorded kernel payload size; short metadata is unavailable.

Volume text reads are bounded to 32 bytes. Trailing NUL padding is omitted;
embedded controls, non-ASCII bytes, quotes, and backslashes use `\xHH` escapes.
Versions/revisions, sectors, and byte counts are decimal; addresses, IDs, flags,
and permissions are hexadecimal. The handoff target is the disc OS kernel,
not a resolved game or `LaunchMe` executable. No on-screen text is rendered.

The `opera-boot: boot XXXXXXXX` diagnostic names the selected kernel before the
final jump; it does not establish that the kernel started. Fatal fault handlers
report `opera-boot: TRAP undf`, `opera-boot: TRAP pabt`, `opera-boot: TRAP dabt`,
or `opera-boot: TRAP addr`, followed by `lr=XXXXXXXX`, and stop.

## Emulator acceptance tools

The acceptance matrix, `test/matrix.sh`, requires successful harness completion,
a kernel-handoff marker, no resident fatal-trap marker, and visible content at
one of its checkpoints. A retained splash after a fatal kernel fault is not a
passing boot. Its optional golden CDCMD comparison uses `test/cmp_reads.py`,
which invokes the sibling disc-label/RomTag inspector, `test/read_rtt.py`.

The matrix and `test/sweep_library.py` share `test/png_visibility.py`. It
reconstructs PNG scanline filters before requiring at least 1000 nonzero decoded
RGB sample bytes. Alpha and filter bytes do not count. Supported screenshots
are 8-bit, noninterlaced RGB or RGBA; malformed or unsupported images report an
error.
Its CLI exits 0 for visible content, 1 for black content, and 2 for an error.
The sweep accepts relative or absolute library paths, resolving image paths
before launching the harness from its own directory.

## Scope limits

The ROM does not verify RSA signatures, execute boot straps, provide an
audio/photo-CD player, support non-CD XBUS devices, issue ReadId, or set
drive speed. Emulator drive-speed and PAL-width settings are external
configuration, not boot-ROM services.
