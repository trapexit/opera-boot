/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * banner.c: the no-disc banner.  The classic four-shape 3DO totem
 * (red diamond, blue rounded square, yellow circle, grey pill) uses
 * the archived SDK-derived color-span table in logo_data.h, plus a
 * blocky white "3DO" wordmark below it.  The third-party logo artwork
 * is not covered by the project's GNU GPL version 2 only license.
 * Linked only into the launch image and painted once at cold boot;
 * the retry loop does not repaint.
 *
 * Framebuffer model: 320x240 16bpp at dram_size+VRAM_BYTES-256K, in the
 * MADAM LRFORM line-pair interleave: pixel (x,y) lives at u16 index
 * (y>>1)*640 + x*2 + (y&1), matching the ROM-installed startup VDL.
 */
#include "opera-boot.h"

#include "banner_caption.h"
#include "logo_data.h"

#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240
#define SCREEN_DEPTH   16
#define SCREEN_BPL     (SCREEN_WIDTH * (SCREEN_DEPTH / 8))
#define VRAM_BYTES     (1024 * 1024)

#define WORD_SCALE   4
#define WORD_COLS    14       /* 3 glyphs x 4 px + 2 single-cell gaps */

#define CAPTION_X      ((SCREEN_WIDTH - CAPTION_WIDTH) / 2)
#define CAPTION_Y      194
#define CAPTION_COLOR  0x7FFF

/* Original 4x7 1bpp glyphs for "3DO", MSB-first per row. */
static const u8 word_rows[3][7] =
  {
    { 0xF0, 0x10, 0x10, 0x70, 0x10, 0x10, 0xF0 },   /* 3 */
    { 0xE0, 0x90, 0x90, 0x90, 0x90, 0x90, 0xE0 },   /* D */
    { 0x60, 0x90, 0x90, 0x90, 0x90, 0x90, 0x60 }    /* O */
  };

static u32 banner_dram_size(void)
{
	u32 msys = MADAM_MSYS;
	u32 b0 = 1u << (((msys >> 4) & 6));
	u32 b1 = 1u << (((msys >> 2) & 6));
	return ((b0 >> 2) + (b1 >> 2)) << 20;
}

static u16 *banner_fb(void)
{
	return (u16 *)(banner_dram_size() + VRAM_BYTES - 256 * 1024);
}

static void fill16(u16 *p, u16 v, u32 words)
{
	while (words--)
		*p++ = v;
}

/* LRFORM-addressed pixel run writer: a line pair is SCREEN_BPL u16s
 * (640); even line = low halfword, odd line = high halfword. */
static void span16(u16 *fb, u32 x, u32 y, u32 n, u16 color)
{
	u32 px = (y >> 1) * SCREEN_BPL + (y & 1) + x * 2;
	while (n-- > 0) {
		fb[px] = color;
		px += 2;
	}
}


static
void
_draw_caption_row(u16      *fb_,
                  const u8 *row_,
                  u32       y_)
{
  u32 x;
  u32 run = 0;

  for(x = 0; x < CAPTION_WIDTH; x++)
    {
      if((row_[(x >> 3)] & (0x80 >> (x & 7))) != 0)
        run++;
      else if(run != 0)
        {
          span16(fb_, ((CAPTION_X + x) - run), y_, run, CAPTION_COLOR);
          run = 0;
        }
    }

  if(run != 0)
    span16(fb_, ((CAPTION_X + CAPTION_WIDTH) - run), y_, run, CAPTION_COLOR);
}


// Startup banner: black with a centered logo, painted once.
void
BannerLogo(void)
{
  u16 *fb = banner_fb();
  const u8 *d = logo_data;
  int i;
  int y;
  int g;
  int r;
  int c;
  int sy;

  fill16(fb, 0, (((u32)SCREEN_BPL * SCREEN_HEIGHT) / 2));

  /* Totem emblem: rows carry count then (x, length, color-index) triplets. */
  for(y = 0; y < LOGO_H; y++)
    {
      i = *d++;
      while(i-- > 0)
        {
          span16(fb,
                 (((SCREEN_WIDTH - LOGO_W) / 2) + d[0]),
                 (44 + (u32)y),
                 d[1],
                 logo_pal[d[2]]);
          d += 3;
        }
    }

  /* Preserve the archived wordmark rasterization and placement. */
  {
    u32 x0 = ((SCREEN_WIDTH - (WORD_COLS * WORD_SCALE)) / 2);
    u32 y0 = ((44 + LOGO_H) + 16);

    for(g = 0; g < 3; g++)
      {
        for(r = 0; r < 7; r++)
          {
            for(c = 0; c < 4; c++)
              {
                if((word_rows[g][r] << c) & 0x80)
                  {
                    for(sy = 0; sy < WORD_SCALE; sy++)
                      span16(fb,
                             (x0 + (u32)(((g * 5) + c) * WORD_SCALE)),
                             (y0 + (u32)((r * WORD_SCALE) + sy)),
                             WORD_SCALE,
                             0x7FFF);
                  }
              }
          }
      }
  }

  for(r = 0; r < CAPTION_HEIGHT; r++)
    _draw_caption_row(fb, caption_rows[r], (CAPTION_Y + (u32)r));
}
