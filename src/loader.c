/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * loader.c: OS/misc/splash component loading from CD and the in-DRAM
 * relocation contract Sherry expects on handoff.
 *
 * Disc model (per tag, vendor semantics):
 *   rt_Size bytes total = payload + 64-byte trailing signature; the
 *   number of blocks read is ceil(rt_Size/2048).  Payload is staged at
 *   STAGE_ADDR, then relocated into the component area that starts at
 *   COMPONENT_BASE and grows upward:
 *     - first file of a component: no round-up
 *     - later files: rounded up to a page (dramSize/64 = 32KB @ 2MB)
 *     - the kernel file additionally gets its _3DO_Stack region
 *       reserved below it
 *     - after each file the load pointer advances by the *live* AIF
 *      image size (RO + RW + zero-init; debug area not counted)
 */
#include "opera-boot.h"

Component g_components[MAX_OS_COMPONENTS];
u32 g_num_components;
u32 g_component_load_addr;

/* Simple, dependency-free word copy/fill (positions are 4-aligned). */
static void u32_copy(u32 *dst, const u32 *src, u32 bytes)
{
	u32 n = bytes >> 2;
	while (n--)
		*dst++ = *src++;
}

static void u32_fill(u32 *dst, u32 bytes)
{
	u32 n = bytes >> 2;
	while (n--)
		*dst++ = 0;
}

/* ceil to 2048-block count, clib-free. */
static u32 blocks_for(u32 bytes)
{
	return (bytes + DISC_BLOCK_SIZE - 1) >> 11;
}

/* DRAM size in bytes - mirrors launch.s read_dram_size (the
 * dipirutils.c FindMemSize math): MSYS bank shifts 4/2, mask 6;
 * stock FZ-1 decodes 1MB + 1MB. */
static u32 dram_size_bytes(void)
{
	u32 msys = MADAM_MSYS;
	u32 b0 = 1u << ((msys >> 4) & 6);
	u32 b1 = 1u << ((msys >> 2) & 6);
	return ((b0 >> 2) + (b1 >> 2)) << 20;
}

/* Bytes actually staged by the last load_component.  The vendor could
 * blind-trust disc records because RSA verified them first; opera-boot
 * skips signature verification by design, so every disc-derived
 * size/address gets bounds-checked against the staged image and the
 * DRAM top instead. */
static u32 g_stage_bytes;

int load_component(u32 first_block, u32 size)
{
	u8 *dst = (u8 *)STAGE_ADDR;
	u32 n;
	u32 i;
  u32 stage_capacity = (dram_size_bytes() - STAGE_ADDR);

  // Bound the original size before sector rounding can overflow. The
  // rounded span must also fit because even the last read writes 2048 bytes.
  if((size == 0) ||
     (size > stage_capacity) ||
     ((blocks_for(size) << 11) > stage_capacity))
    {
      kprint(OPERA_BOOT_LOG_PREFIX "badsize\r\n");
      return -1;
    }

	g_stage_bytes = size;
	n = blocks_for(size);
	for (i = 0; i < n; i++) {
		mei_wait_lines(1);
		if (mei_read_block(first_block + i, dst) < 0)
			return -1;
		dst += DISC_BLOCK_SIZE;
	}
	return 0;
}

static u32 add_component(u32 addr, u32 size)
{
	g_components[g_num_components].comp_Address = addr;
	g_components[g_num_components].comp_Size = size;
	g_num_components++;
	return addr;
}

/* Relocate the component staged at STAGE_ADDR.
 *
 * Placement rule (grounded by all six retail matrices): every file
 * record carries its mastered load address, and it is authoritative.
 * The pre-v24 kernels are *not* self-relocating (aif_blSelfReloc is a
 * NOP) and absolutely linked (kernel at 0x10000); the v24 kernels
 * self-relocate and tolerate any staging position.  Honouring the
 * record address satisfies both.  Mastering has already folded the
 * kernel's _3DO_Stack reserve into the recorded address (return-fire's
 * v23 kernel masters sherry at 0x18000 with b3_Stack=0x2800 and boots
 * with no bump), so no stack reserve is ever applied here.
 *
 * There is deliberately no addr==0 fallback branch: the vendor's
 * RelocateOsComponent walks the executable list with
 * `while ((addr = *image++) != 0)`, i.e. a zero address IS the list
 * terminator, so a record carrying addr==0 can never be seen in-loop.
 * (The vendor additionally forces addr=0 for the FIRST file and takes
 * its running-pointer + page-round (dramSize/64) + stack-bump path,
 * which computes to the same address the record carries; we keep the
 * record side, proven identical by the matrix.) */
int
relocate_component(void)
{
  enum
    {
      COMPONENT_PRELUDE_BYTES = 8,
      COMPONENT_RECORD_HEADER_BYTES = 8
    };
  const u32 *img = (const u32 *)(STAGE_ADDR + COMPONENT_PRELUDE_BYTES);
  u32 first_component = g_num_components;
  u32 top = dram_size_bytes();
  u32 stage_end = (STAGE_ADDR + g_stage_bytes);
  u32 stage_top = (STAGE_ADDR + (blocks_for(g_stage_bytes) << 11));

  if(g_stage_bytes < (COMPONENT_PRELUDE_BYTES + sizeof(u32)))
    {
      kprint(OPERA_BOOT_LOG_PREFIX "badrec\r\n");
      return -1;
    }

  // Each iteration is bounded by the declared bytes and the record limit.
  while(1)
    {
      u32 rec_addr;
      u32 rec_size;
      u32 dst;
      u32 aif_live;

      if(((u32)img > stage_end) || ((stage_end - (u32)img) < sizeof(u32)))
        {
          kprint(OPERA_BOOT_LOG_PREFIX "badrec\r\n");
          return -1;
        }

      rec_addr = *img;
      if(rec_addr == 0)
        {
          // A previous misc component cannot make an empty OS list valid.
          if(g_num_components == first_component)
            {
              kprint(OPERA_BOOT_LOG_PREFIX "badrec\r\n");
              return -1;
            }

          return 0;
        }

      if((stage_end - (u32)img) < COMPONENT_RECORD_HEADER_BYTES)
        {
          kprint(OPERA_BOOT_LOG_PREFIX "badrec\r\n");
          return -1;
        }

      img++;
      rec_size = *img++;

      // Disc records have no signature backing: retain the AIF, destination,
      // DRAM, staging-overlap and declared-payload bounds before copying.
      // The top guard precedes rec_addr + rec_size, so that sum cannot wrap.
      if((rec_size < AIF_SIZEOF) ||
         (rec_addr < COMPONENT_BASE) || (rec_addr >= top) ||
         (rec_size > (top - rec_addr)) ||
         ((rec_addr < stage_top) && ((rec_addr + rec_size) > STAGE_ADDR)) ||
         (((u32)(const u8 *)img + rec_size) > stage_end))
        {
          kprint(OPERA_BOOT_LOG_PREFIX "badrec\r\n");
          return -1;
        }

      g_component_load_addr = rec_addr;
      if(g_num_components >= MAX_OS_COMPONENTS)
        return -1;
      dst = add_component(g_component_load_addr, rec_size);

      u32_copy((u32 *)dst, img, rec_size);
      img = (const u32 *)((const u8 *)img + rec_size);

      aif_live = ((((const u32 *)dst)[AIF_RO_SIZE] +
                   ((const u32 *)dst)[AIF_RW_SIZE]) +
                  ((const u32 *)dst)[AIF_ZI_SIZE]);
      g_component_load_addr = (dst + aif_live);
    }
}

/* Early C&B-era masters carry only RSA_OLD_MISCCODE: their executable
 * list/mem/divide utilities ride
 * inside the compressed OS image.  Running the disc's self-expanding
 * unpacker here wedges under Opera (its later relocation passes walk
 * writes past DRAM top), so instead of harvesting bodies we publish our
 * own clean-room equivalents: ob_build_old_misc() lays down the two-word
 * prelude plus the 14-slot entry table into this parked buffer, with
 * every slot pointing into this blob - which survives boot because the
 * kernel preserves low DRAM (it calls back into resident dipir code at
 * runtime).  Slot order is the disc-side boot contract; semantics are
 * the documented Portfolio interfaces (see src/oldmisc.c). */
void relocate_old_misc(void)
{
	u32 miscaddr;

	if (g_num_components >= MAX_OS_COMPONENTS) {
		kprint(OPERA_BOOT_LOG_PREFIX "miscfull\r\n");
		return;
	}
	/* Park the rebuild in the dead band below COMPONENT_BASE: with
	 * relocate_component's record floor (>= COMPONENT_BASE) a
	 * disc-honored kernel record can then never overwrite it -
	 * era kernels are mastered at 0x10000.  The running load pointer
	 * deliberately does not advance. */
	miscaddr = add_component(OLDMISC_ADDR, OLDMISC_SPAN);
	ob_build_old_misc(miscaddr);
}

void clear_gaps(void)
{
	u32 i;
	for (i = 0; i + 1 < g_num_components; i++) {
		u32 end = g_components[i].comp_Address +
			  g_components[i].comp_Size;
		u32 next = g_components[i + 1].comp_Address;
		if (next < end)
			return;
		u32_fill((u32 *)end, next - end);
	}
}

/* Point vector 0x10 at the dabt net for the duration of a boot
 * attempt (same encoding idiom as ob_install_traps).  The previous
 * vector word is saved: on runtime re-entries it is the *kernel's*
 * dabt handler, which must be put back before returning to a live OS. */
static u32 saved_dabt_word;
static int dabt_net_armed;

void ob_arm_dabt_net(void)
{
	extern char ob_dabt_net[];
	u32 off = ((u32)ob_dabt_net - 0x10 - 8) >> 2;
	saved_dabt_word = *(volatile u32 *)0x10;
	*(volatile u32 *)0x10 = 0xEA000000u | (off & 0x00FFFFFFu);
	dabt_net_armed = 1;
}

void ob_disarm_dabt_net(void)
{
	g_dabt_pad = 0;
	if (dabt_net_armed) {
		*(volatile u32 *)0x10 = saved_dabt_word;
		dabt_net_armed = 0;
	}
}

/* Write branch vectors over the launch ROM's spin-vectors so an
 * early kernel fault reports instead of hanging. */
void ob_install_traps(void)
{
	extern char ob_trap_undef[], ob_trap_pabt[], ob_trap_dabt[],
		    ob_trap_addr[];
	static const u8 vec[4] = { 0x04, 0x0C, 0x10, 0x14 };
	const char *const fn[4] = { ob_trap_undef, ob_trap_pabt,
				    ob_trap_dabt, ob_trap_addr };
	u32 i;
	for (i = 0; i < 4; i++) {
		u32 off = ((u32)fn[i] - vec[i] - 8) >> 2;
		*(volatile u32 *)vec[i] = 0xEA000000u | (off & 0x00FFFFFFu);
	}
}

void boot_kernel(u32 kern, u32 r6, u32 r7, u32 r8)
{
	ob_install_traps();
	/* Handoff overwrites the net's 0x10 vector with the report traps;
	 * just disarm the pad - never restore the saved word here. */
	g_dabt_pad = 0;
	dabt_net_armed = 0;
	/* Mask all four CLIO interrupt windows and drop latched pendings;
	 * the kernel enables its own set as it initializes.  Note the
	 * register layout pitfall this pays down: ClrIntEnBits lives at
	 * +0x4C/+0x6C, NOT +0x48/+0x68 (those SET enables). */
	CLIO_CLRINT0EN = 0xFFFFFFFF;
	CLIO_CLRINT1EN = 0xFFFFFFFF;
	CLIO_CLRINT0 = 0xFFFFFFFF;
	CLIO_CLRINT1 = 0xFFFFFFFF;
	*(volatile u32 *)MAGIC_KERNELADR = kern;
	ob_kernel_tail(kern, r6, r7, r8);
}
