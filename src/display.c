/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * display.c: boot-time screen painting.  The FZ-1-class machine this
 * ROM targets has 1MB VRAM; the ROM-installed startup VDL (built by
 * install_startup_vdl) scans the top 256KB of the combined DRAM+VRAM
 * aperture (0x2C0000 on a stock 2MB unit) as a 320x240x16bpp frame, and
 * dipir's job is to keep it painted while the OS loads.
 */
#include "opera-boot.h"

#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240
#define SCREEN_DEPTH   16
#define SCREEN_BPL     (SCREEN_WIDTH * (SCREEN_DEPTH / 8))
#define VRAM_BYTES     (1024 * 1024)

static u32 dram_size(void);

/* The ROM's own startup VDL: structure copied verbatim from the table
 * Opera pre-seeds at VRAM offset 0xB0000 (head + 32-entry grey CLUT
 * ramp + per-field control words pointing at the 0xC0000 framebuffer).
 * Embedded pointers are combined-aperture-absolute; hardware consumes
 * them as-is, Opera masks them with 0xFFFFF.  On a stock 2 MB FZ-1 the
 * table below is bit-identical to what the core plants, so emulated
 * frames do not change. */
#define VDL_VRAM_OFF   0xB0000
#define STOCK_DRAM     (2u * 1024 * 1024)
#define MADAM_VDL_HEAD (*(volatile u32 *)(MADAM_BASE + 0x580))

static const u32 startup_vdl[46] = {
	0x00004410, 0x002C0000, 0x002C0000, 0x002B0098,
	0x00000000, 0x01080808, 0x02101010, 0x03191919,
	0x04212121, 0x05292929, 0x06313131, 0x073A3A3A,
	0x08424242, 0x094A4A4A, 0x0A525252, 0x0B5A5A5A,
	0x0C636363, 0x0D6B6B6B, 0x0E737373, 0x0F7B7B7B,
	0x10848484, 0x118C8C8C, 0x12949494, 0x139C9C9C,
	0x14A5A5A5, 0x15ADADAD, 0x16B5B5B5, 0x17BDBDBD,
	0x18C5C5C5, 0x19CECECE, 0x1AD6D6D6, 0x1BDEDEDE,
	0x1CE6E6E6, 0x1DEFEFEF, 0x1EF8F8F8, 0x1FFFFFFF,
	0xE0010101, 0xC001002C, 0x002180EF, 0x002C0000,
	0x002C0000, 0x002B00A8, 0x00000000, 0x002C0000,
	0x002C0000, 0x002B0000
};

/* Build + arm the startup VDL.  Called only on a full boot attempt:
 * runtime same-disc re-entries must not touch 0x2B0000 - the live OS
 * may own that VRAM - and must not rewrite the OS's VDL head.  Pointer
 * words are patched by index, not by value range: 0x002180EF sits in
 * the aperture's numeric range but is a control word. */
void install_startup_vdl(void)
{
	u32 base = dram_size();
	u32 delta = base - STOCK_DRAM;
	volatile u32 *dst = (volatile u32 *)(base + VDL_VRAM_OFF);
	static const u8 ptr_idx[9] = { 1, 2, 3, 39, 40, 41, 43, 44, 45 };
	int i;

	for (i = 0; i < 46; i++)
		dst[i] = startup_vdl[i];
	if (delta != 0)
		for (i = 0; i < 9; i++)
			dst[ptr_idx[i]] += delta;
	MADAM_VDL_HEAD = base + VDL_VRAM_OFF;
}

static u32 dram_size(void)
{
	u32 msys = MADAM_MSYS;
	u32 b0 = 1u << (((msys >> 4) & 6));
	u32 b1 = 1u << (((msys >> 2) & 6));
	return ((b0 >> 2) + (b1 >> 2)) << 20;
}

static u16 *video_buf(void)
{
	return (u16 *)(dram_size() + VRAM_BYTES - 256 * 1024);
}

static void fill16(u16 *p, u16 v, u32 words)
{
	while (words--)
		*p++ = v;
}

static u32 get_field(const u8 *buf, u32 index, u32 bits)
{
	if (bits <= 8) {
		u32 i = index * bits;
		return (buf[i >> 3] >> (8 - bits - (i & 7))) &
		       ((1u << bits) - 1);
	} else {
		u32 i = index * bits;
		const u16 *w = (const u16 *)buf;
		u32 off = i >> 4;
		u32 sh = 16 - bits - (i & 15);
		return (w[off] >> sh) & ((1u << bits) - 1);
	}
}

/* Truncating signed halve without a division libcall. */
static s32 half(s32 v)
{
	return (v < 0) ? -((-v) >> 1) : (v >> 1);
}

// Blit a bounded packed-pixel image centered on the 320x240 screen.
// Lines advance in pairs to match the hardware line interleave. Invalid
// optional images return 0 after blacking the framebuffer.
int
display_image(const void *image_,
              u32         image_size_,
              const u8   *pattern_)
{
  const VideoImage *image = (const VideoImage *)image_;
  const u8 *picture;
  u16 *videobuf = video_buf();
  u32 colorscale;
  s32 leftmargin, topmargin;
  s32 lines;
  u32 i;
  u32 bytes_per_picture_line;
  u32 line_bits;
  u32 pair_bytes;
  u32 pair_count;
  u32 payload_bytes;
  u32 last_pair_half;

	/* Vendor DefaultDisplayImage blacks the framebuffer BEFORE it
	 * validates the header: a corrupt splash on a booting disc shows
	 * black, not a stale banner.  Same order here. */
	fill16(videobuf, 0, (u32)SCREEN_BPL * SCREEN_HEIGHT / 2);

  if((image_ == NULL) || (pattern_ == NULL) ||
     (image_size_ < sizeof(VideoImage)))
    return 0;
  if(((u32)image_ & (sizeof(u32) - 1)) != 0)
    return 0;

  for(i = 0; i < sizeof(image->vi_Pattern); i++)
    if(image->vi_Pattern[i] != pattern_[i])
      return 0;
  if((image->vi_Depth == 0) || (image->vi_Depth > SCREEN_DEPTH) ||
     ((image->vi_Depth & (image->vi_Depth - 1)) != 0))
    return 0;
  if((image->vi_Width == 0) || (image->vi_Height == 0))
    return 0;

  // Supported depths are 1/2/4/8/16. Keep the existing truncated line
  // stride, even for subbyte rows: each pair reads 2 * width pixels,
  // then advances by 2 * floor(width * depth / 8) bytes.
  line_bits = ((u32)image->vi_Width * image->vi_Depth);
  bytes_per_picture_line = (line_bits >> 3);
  pair_bytes = ((line_bits + 3) >> 2);
  pair_count = (((u32)image->vi_Height + 1) >> 1);
  payload_bytes = (image_size_ - sizeof(VideoImage));
  if(pair_bytes > payload_bytes)
    return 0;

  // Odd heights still read a complete final pair. Its offset, halved,
  // fits u32: (32768 - 1) * (65535 * 2) < 2^32. Compare before
  // doubling so giant 16bpp dimensions cannot wrap the byte extent.
  last_pair_half = ((pair_count - 1) * bytes_per_picture_line);
  if(last_pair_half > ((payload_bytes - pair_bytes) >> 1))
    return 0;

  picture = ((const u8 *)image_ + sizeof(VideoImage));
  colorscale = ob_udiv((1u << SCREEN_DEPTH),
                      ((1u << image->vi_Depth) - 1));

	leftmargin = half(SCREEN_WIDTH - (s32)image->vi_Width);
	topmargin = half(SCREEN_HEIGHT - (s32)image->vi_Height);
	if (topmargin & 1)
		topmargin++;
	if (topmargin >= 0) {
		videobuf += topmargin * (SCREEN_BPL / 2);
		lines = image->vi_Height;
		/* Over-height headers (e.g. vi_Height = 243 with topmargin
		 * rounding to 0) would pair-write past the visible grid;
		 * the vendor lets those land in the off-visible pad,
		 * clamping here is strictly safer. */
		if (lines > SCREEN_HEIGHT)
			lines = SCREEN_HEIGHT;
	} else {
		picture += (u32)(-topmargin) * bytes_per_picture_line;
		lines = SCREEN_HEIGHT;
	}

	for (; lines > 0; lines -= 2) {
		u32 pix, ln, color;
		if(image->vi_Width <= SCREEN_WIDTH) {
			for (pix = 0; pix < image->vi_Width; pix++)
				for (ln = 0; ln < 2; ln++) {
					color = get_field(picture,
						2 * pix + ln, image->vi_Depth);
					color *= colorscale;
					if (color > 0xFFFF)
						color = 0xFFFF;
					videobuf[2 * (pix + (u32)leftmargin) + ln] =
						(u16)color;
				}
		} else {
			for (pix = 0; pix < SCREEN_WIDTH; pix++)
				for (ln = 0; ln < 2; ln++) {
					color = get_field(picture,
						2 * (pix + (u32)-leftmargin) + ln,
						image->vi_Depth);
					color *= colorscale;
					if (color > 0xFFFF)
						color = 0xFFFF;
					videobuf[2 * pix + ln] = (u16)color;
				}
		}
		videobuf += SCREEN_BPL;		/* advance two lines (u16s) */
		picture += bytes_per_picture_line * 2;
	}
	return 1;
}
