# Boot architecture

The following sequence describes the implemented boot contract, including
the disc formats and register state needed to reproduce it in another ROM
or emulator. All CPU addresses and multibyte disc fields below are
big-endian unless explicitly stated otherwise. Sector addresses are physical
CD frame numbers, not zero-based ISO offsets.

[Boot reference](BOOT_REFERENCE.md) supplies the full hardware-register,
label, display, legacy-misc, and resident-service layouts.

## 1. ROM layout and cold reset

The CPU resets at address zero with ROM aliased over low memory. The first
instruction loads the permanent ROM entry at `0x03000008`; that entry writes
zero to reserved DRAM scratch word `0x3C` to release the overlay before
reading persistent boot state. The zero-filled ROM image has these slots:

| File offset | CPU address | Contents |
| --- | --- | --- |
| `0x00000` | `0x03000000` | Reset and warm-reset launcher |
| `0x01000` | `0x03001000` | ROM RomTag table |
| `0x02000` | `0x03002000` | Resident boot code, linked for DRAM `0x200` |
| `0x1F000` | `0x0301F000` | 256-byte zero-version placeholder kernel header |

The ROM table advertises subsystem `0x10`, a `ROM_DIPIR` record of type
`0x20` with RAM destination `0x200`, and a `ROM_KERNEL_CD` record of type
`0x31` referring to the placeholder header. A zero record terminates it.

Cold reset enters SVC mode with IRQ and FIQ masked (`CPSR` control `0xD3`),
zeros DRAM `[0, 0x10000)`, installs the exception vector page, and copies the
resident image from ROM to `0x200`, rounding its copy length to four bytes.
The initial reset/undefined/prefetch-abort/data-abort/address-error vectors
loop; SWI returns with `movs pc,lr`, and IRQ/FIQ return with
`sub pc,lr,#4` (`0xE24EF004`, without SPSR restoration).

The launcher publishes these low-memory cells:

| DRAM address | Meaning at cold boot |
| --- | --- |
| `0x28` | Current kernel AIF address, initially `0x0301F000` |
| `0x2C` | ROM RomTag table, `0x03001000` |
| `0x30` | Warm-reset entry in the launcher |
| `0x34` | Initialized-state sentinel, `0xB00710AD` |
| `0x38` | Timing family: `0` NTSC, `1` PAL |
| `0x3C` | Overlay-release scratch word, zeroed on every launcher entry |
| `0x40` | Pending resident DIPIR-reset continuation, normally zero |

DRAM size is decoded from MADAM `MSYS` at `0x03300004`. For register value
`v`, its size in bytes is:

```text
(((1 << ((v >> 4) & 6)) >> 2) + ((1 << ((v >> 2) & 6)) >> 2)) << 20
```

The stock configuration has 2 MiB DRAM followed by 1 MiB VRAM. Cold reset
sets banked stack tops to DRAM top minus `0x10000` (FIQ), `0x11000` (IRQ),
`0x13000` (ABT), and `0x14000` (UND), using a temporary SVC stack at
`top - 0x12000`. The final SVC stack is `top - 16`.

Timing detection reads CLIO `VCNT` at `0x03400034`, masked with `0x7FF`.
A line number of at least 264 selects PAL; two counter wraps without such a
line select NTSC. Detection stops after `0x100000` samples with an NTSC
fallback. The result is cached at `0x38`.

The launcher paints the startup banner and calls resident vector 0 at
`0x200`; it does not fabricate XBUS DIPIR latches. A returned boot attempt
is retried after 3150 scanline counter changes, approximately 200 ms. The
banner is painted only once.

## 2. Bus attachment and full-attempt hardware setup

The resident entry saves the caller's SP/LR and switches to its private
stack. It selects MEI unit 0 through CLIO XBUS:

| Address | Register / relevant bits |
| --- | --- |
| `0x03400400` | Expansion control; `CPUHASXBUS = 0x80` |
| `0x03400500` | Unit select |
| `0x03400540` | Poll; status valid `0x10`, data valid `0x20`, media access `0x80`, reset `0x08` |
| `0x03400580` | Command/status FIFO |
| `0x034005C0` | Data FIFO |

Attachment waits for bus ownership, acknowledges media access by writing
POLL bit `0x80`, and drains stale status. If either DIPIR latch has CURRENT
bit `0x8000`, the ROM first writes `SOFTRES | CLRDIP` (`0x30`) to CLIO
CSTAT at `0x03400028`. This resets the CPU and XBUS controller, converting
CURRENT to before-reset bit `0x4000` without resetting the external drive.
The helper saves ADBIO and clears its ROM2-select bit `0x04` so reset fetches
the boot ROM, then restores the caller's ADBIO on resumption. The launcher
resumes the saved resident invocation before any FIFO access; the acknowledged
drive does not require another reset. Attachment and
command polling have a 4,000,000-iteration limit.
Commands are seven bytes written to the command/status FIFO. Replies have a
16-byte status limit; the final status byte's `0x10` bit denotes a drive
error. A sector read consumes all 2048 pending data bytes when data-valid
appears, without polling between individual bytes.

On a full boot attempt, the ROM enables MADAM video context (`MCTL` bit
`0x4000`) and then CLUT DMA (`0x2000`), using the safe scanline window
`10 <= VCNT < 13` with a field boundary between enables. It saves CLIO
ADBIO and timer slack, programs timer 14 for a 1 ms cadence, sets slack to
336, and sets ADBIO to `(saved | 0x40) & ~0x04`.

It installs its own startup VDL at `DRAM_size + 0xB0000`, with framebuffer
at `DRAM_size + 0xC0000`, and publishes the VDL through MADAM `0x03300580`.
The 320×240 framebuffer uses 16-bit pixels in paired-line
LRFORM order: pixel `(x,y)` is at halfword index
`(y >> 1) * 640 + x * 2 + (y & 1)`. The exact VDL words are in the
[display reference](BOOT_REFERENCE.md#display-and-splash-format).

## 3. Drive readiness and disc discovery

Poll device status with command `0x82` up to 80 times, spacing not-ready
replies by approximately 50 ms (787 scanline changes). The status must report
door closed (`0x80`) and disc present (`0x40`); ready is bit `0x01`.
If spinning bit `0x20` is clear, issue spin-up command `0x02`.

Read disc information (`0x8B`), session information (`0x8D`), and track 1's
TOC (`[0x8C, 0, 1, 0, 0, 0, 0]`). Accept disc ID zero, first track 1,
last track at least 1, and no multisession flag (`0x80`). Track 1 must be
data (`AddrCntrl & 0x04`) with neither audio preemphasis (`0x01`) nor
four-channel audio (`0x08`).

Derive the first physical block from the TOC's binary minute/second/frame:

```text
first_block = minute * 4500 + second * 75 + frame
```

A normal image starts at physical block 150, but this value is not hardcoded.
To read physical block `B`, send:

```text
[0x10, B / 4500, (B % 4500) / 75, B % 75, 0, 0, 1]
```

## 4. Volume label and component table

Read 2048-byte sectors at `first_block + 0`, `+225`, `+0`, then `+33011`
until a label validates. The label occupies the first 132 bytes of its
sector. Acceptance requires record type 1 followed by five `0x5A` sync
bytes; structure version, volume flags, and filesystem fields are not
additional acceptance checks.

The label's 32-byte volume identifier is at byte 40, volume block size at
byte 76, and block count at byte 80. The loader does not walk the root
directory or search for an OS filename.

If the accepted label is at physical block `L`, read the single RomTag
sector at `R = L + 1`. It holds up to 64 32-byte records. Each has eight
one-byte fields (subsystem, type, version, revision, flags, type-specific,
reserved, reserved), followed by `u32 offset`, `u32 size`, and four reserved
`u32` words. Subsystem zero terminates the table; at least two preceding
records are required.

Select the first matching record of each relevant type under subsystem
`0x0F`:

| Type | Component |
| --- | --- |
| `0x07` | Required OS |
| `0x10` | Modern misc code |
| `0x0F` | Legacy misc, used only if modern misc is absent |
| `0x14` | Optional application splash |
| `0x17` | Optional device permissions, from its first reserved `u32` word |

An OS and one misc variant are required. Component sector addresses are
`R + offset`, not offsets from the track or label. Sizes are in bytes and
include the conventional 64-byte signature trailer; that trailer is neither
verified nor subtracted. Boot-strap and overlay tags are not executed.

## 5. Staging and executable relocation

Read each component into DRAM `0xA0000`, in complete 2048-byte sectors.
Before reading, reject zero byte lengths, lengths greater than
`DRAM_top - 0xA0000`, and lengths whose sector-rounded span exceeds that
capacity. The original length is checked before adding 2047 for rounding.

The component order is:

1. Read and relocate modern misc. For legacy misc, read the OS once and
   construct the resident legacy-misc interface instead.
2. Read and display the optional application splash, if present and nonempty.
3. Read the OS and relocate its executable list.

Each modern misc or OS component begins with an eight-byte prelude, followed
by a word-aligned list:

```text
u32 destination, u32 byte_size, byte payload[byte_size]
... more records ...
u32 zero_destination
```

Each required list must contribute at least one executable. Record addresses,
headers, payloads, and the zero terminator must fit within the declared
component byte length, not merely its sector padding. Bytes after a valid
terminator may contain padding or signatures. Across misc and OS there can
be at most eight executable components.

A payload must contain at least the 128-byte AIF header. Its mastered
destination must be at least `0x10000`, fit below DRAM top, and not overlap
the sector-rounded staging span. The loader copies words to that address;
it does not choose a new base or add a kernel-stack reservation. The format
expects word-aligned addresses and payload sizes.

AIF RO, RW, and ZI sizes (header words 5, 6, and 8) update the running load
pointer, with 1 KiB rounding before OS relocation. They do not override the
next record's destination. The first executable from the OS list is the
kernel; the first component overall supplies the misc address. The loader
clears ascending gaps between copied components, stopping at an overlap.

Legacy `RSA_OLD_MISCCODE` uses a 3072-byte component at `0xC000`: instructions
`MOV r0,pc` and `MOV pc,r8`, followed by 14 absolute routine pointers.
The resident routines implement memory copying/filling, list operations,
and signed/unsigned division, including divide-by-ten. Their exact slot and
register ABI is in the [legacy misc reference](BOOT_REFERENCE.md#old-misc-abi).
The disc's self-expanding misc unpacker is not executed by the loader.

A splash has a 24-byte `VideoImage` header with the seven-byte `APPSCRN`
pattern, positive dimensions, and depth 1, 2, 4, 8, or 16. Packed pixels are
MSB-first and advance in paired rows. The declared loaded bytes must contain
the complete final pair, including for odd heights. Valid images are centered
and cropped to the framebuffer. Missing or unreadable artwork is ignored;
a loaded malformed image leaves a black framebuffer without blocking OS boot.

## 6. Kernel handoff

Restore saved timer slack and ADBIO as `saved | 0x40`; timer programming is
not undone. Save the label identity and track start for subsequent resident
calls. The kernel's `_3DOBinHeader` starts at AIF offset `0x80`; its major
version byte is at kernel offset `0x94`.

| Kernel major version | `r6` | `r7` | `r8` |
| --- | --- | --- | --- |
| Below 24 | `0x100` | Pointer to the resident `DiscLabel` | Misc component address |
| 24 or above | `0x101` | Pointer to resident `BootData` | Misc component address |

`BootData` consists of three 32-bit words: version 1, label pointer, and
device permissions (zero when the permissions tag is absent).

Install early-fault vectors, disarm the boot-attempt abort recovery, clear
both CLIO interrupt-enable banks (`0x0340004C`, `0x0340006C`) and pending
banks (`0x03400044`, `0x03400064`) with `0xFFFFFFFF`, and publish the kernel
address at `0x28`. Set `r6`–`r8` and jump directly to the kernel's AIF start,
not a decoded entrypoint. The kernel's own AIF instructions perform any
required initialization before entering the OS.

## 7. Runtime calls, warm reset, and failures

The resident table remains at `0x200`. Its 11 entries provide boot re-entry,
two RSA-check functions that return 1, ROM system-info query/set, sector ECC,
a literal entry count, ROM-file open/read/close, and the current OS version.
ECC and ROM-file operations return zero. The OS-version service returns
`(major << 8) | revision`.

`QueryROMSysInfo` returns scalar values in `r0`, leaving any output buffer
untouched. Tag `0x10001` returns 60 for NTSC or 50 for PAL; tag `0x20004`
returns `0x10101` or `0x20202` respectively. Other queries, invalid cached
timing values, and all `SetROMSysInfo` calls return `-2`. PAL1 and PAL2 share
the same timing-family answers.

Runtime vector-0 re-entry attaches to MEI and probes a label using the saved
track start. An equal 32-byte volume identifier and block count return to
the caller without changing the running OS's video, timer, or ADBIO state.
This is a content comparison, not authentication. A different disc takes
the full boot path. The published label remains unchanged during a
same-disc probe.

A pending resident DIPIR reset is handled before ordinary warm reset:
with the sentinel valid, CSTAT reset-reason bit `0x40` set, and a nonzero
continuation at `0x40`, the launcher returns to that continuation using
`movs pc,r2`. The helper clears the pending cell and restores its saved
registers, caller stack, and ROM-bank selection; it does not jump to the
kernel or rerun cold initialization.

Otherwise, when the sentinel is valid and `0x28` names a loaded kernel,
the launcher resets only the SVC stack to `DRAM_top - 16` and jumps to
that kernel with the caller's `r6`–`r8`, preserving cached timing. Without
such a kernel it follows cold reset.

Ordinary drive, label, table, or required-component failures restore any
saved ADBIO/slack state and the caller's data-abort vector, then return to
the entry caller. Cold-launch calls retry; runtime calls return to the OS.
A caught data abort clears MADAM's abort latch, asserts
XBUS reset for 1575 scanline changes, releases it for 9450 changes, and
returns through the same cleanup path. Unhandled or early-kernel faults
report a trap and stop. Scanline-based waits require a running video counter;
unlike command polling and timing detection, they have no iteration timeout.

