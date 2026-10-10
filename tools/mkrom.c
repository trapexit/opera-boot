/*
 * opera-boot (GPL-2.0-only): ROM image packer.
 *
 * Layout (must match Makefile and launch/romtags.s):
 *   0x00000  launch.bin
 *   0x01000  romtags.bin
 *   0x02000  minidipir.bin
 *   0x1F000  fake kernel header (256 zero bytes, current OS v0.0)
 *   pad with zeros to 131072 bytes.
 *
 * Prints size/crc32/md5/sha1 for release metadata.
 *
 * Usage: mkrom out.bin launch.bin romtags.bin minidipir.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ROM_SIZE_DEFAULT 131072

#define SLOT_LAUNCH  0x00000
#define SLOT_ROMTAGS 0x01000
#define SLOT_DIPR    0x02000
#define SLOT_FAKEHDR 0x1F000

static uint32_t crc_table[256];
static void crc32_init(void)
{
	uint32_t c;
	int i, k;
	for (i = 0; i < 256; i++) {
		c = (uint32_t)i;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
}
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	size_t i;
	for (i = 0; i < n; i++)
		c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

/* --- tiny MD5 (RFC 1321), standard test-verified ----------------------- */
typedef struct { uint32_t a,b,c,d; uint64_t len; uint8_t buf[64]; size_t n; } md5_t;
static uint32_t rol(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static const uint32_t K[64] = {
	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
	0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
	0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
	0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
	0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
	0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const uint8_t S[64] = {
	7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
	5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
	4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
	6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
static void md5_block(md5_t *m, const uint8_t b[64])
{
	uint32_t A = m->a, B = m->b, C = m->c, D = m->d, w[16];
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)b[i*4] | ((uint32_t)b[i*4+1] << 8) |
		       ((uint32_t)b[i*4+2] << 16) | ((uint32_t)b[i*4+3] << 24);
	for (i = 0; i < 64; i++) {
		uint32_t f, g;
		if (i < 16)      { f = (B & C) | (~B & D); g = (uint32_t)i; }
		else if (i < 32) { f = (D & B) | (~D & C); g = (uint32_t)(1 + 5*i) & 15; }
		else if (i < 48) { f = B ^ C ^ D;          g = (uint32_t)(5 + 3*i) & 15; }
		else             { f = C ^ (B | ~D);       g = (uint32_t)(7*i) & 15; }
		f += A + K[i] + w[g];
		A = D; D = C; C = B;
		B += rol(f, S[i]);
	}
	m->a += A; m->b += B; m->c += C; m->d += D;
}
static void md5_init(md5_t *m)
{ m->a = 0x67452301; m->b = 0xEFCDAB89; m->c = 0x98BADCFE; m->d = 0x10325476; m->len = 0; m->n = 0; }
static void md5_update(md5_t *m, const uint8_t *p, size_t n)
{
	m->len += n;
	while (n) {
		size_t take = 64 - m->n;
		if (take > n) take = n;
		memcpy(m->buf + m->n, p, take);
		m->n += take; p += take; n -= take;
		if (m->n == 64) { md5_block(m, m->buf); m->n = 0; }
	}
}
static void md5_final(md5_t *m, uint8_t out[16])
{
	uint64_t bits = m->len * 8;
	uint8_t one = 0x80, zero = 0, le[8];
	int i;
	for (i = 0; i < 8; i++) le[i] = (uint8_t)(bits >> (8*i));
	md5_update(m, &one, 1);
	while (m->n != 56) md5_update(m, &zero, 1);
	md5_update(m, le, 8);
	out[0]  = (uint8_t)m->a; out[1]  = (uint8_t)(m->a >> 8);
	out[2]  = (uint8_t)(m->a >> 16); out[3]  = (uint8_t)(m->a >> 24);
	out[4]  = (uint8_t)m->b; out[5]  = (uint8_t)(m->b >> 8);
	out[6]  = (uint8_t)(m->b >> 16); out[7]  = (uint8_t)(m->b >> 24);
	out[8]  = (uint8_t)m->c; out[9]  = (uint8_t)(m->c >> 8);
	out[10] = (uint8_t)(m->c >> 16); out[11] = (uint8_t)(m->c >> 24);
	out[12] = (uint8_t)m->d; out[13] = (uint8_t)(m->d >> 8);
	out[14] = (uint8_t)(m->d >> 16); out[15] = (uint8_t)(m->d >> 24);
}

/* --- tiny SHA-1 -------------------------------------------------------- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_t;
static uint32_t rol32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static void sha1_block(sha1_t *s, const uint8_t b[64])
{
	uint32_t w[80], a, bb, c, d, e, f, k, t;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
		       ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
	for (i = 16; i < 80; i++)
		w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	a = s->h[0]; bb = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20)      { f = (bb & c) | (~bb & d);      k = 0x5A827999; }
		else if (i < 40) { f = bb ^ c ^ d;                k = 0x6ED9EBA1; }
		else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
		else             { f = bb ^ c ^ d;                k = 0xCA62C1D6; }
		t = rol32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol32(bb, 30); bb = a; a = t;
	}
	s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(sha1_t *s)
{ s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE; s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0; s->len=0; s->n=0; }
static void sha1_update(sha1_t *s, const uint8_t *p, size_t n)
{
	s->len += n;
	while (n) {
		size_t take = 64 - s->n;
		if (take > n) take = n;
		memcpy(s->buf + s->n, p, take);
		s->n += take; p += take; n -= take;
		if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
	}
}
static void sha1_final(sha1_t *s, uint8_t out[20])
{
	uint64_t bits = s->len * 8;
	uint8_t one = 0x80, zero = 0, be[8];
	int i;
	for (i = 0; i < 8; i++) be[i] = (uint8_t)(bits >> (56 - 8*i));
	sha1_update(s, &one, 1);
	while (s->n != 56) sha1_update(s, &zero, 1);
	sha1_update(s, be, 8);
	for (i = 0; i < 5; i++) {
		out[i*4]   = (uint8_t)(s->h[i] >> 24);
		out[i*4+1] = (uint8_t)(s->h[i] >> 16);
		out[i*4+2] = (uint8_t)(s->h[i] >> 8);
		out[i*4+3] = (uint8_t)s->h[i];
	}
}

static void place(uint8_t *img, const char *path, unsigned off, unsigned limit)
{
	FILE *f = fopen(path, "rb");
	long sz;
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || (long)off + sz > (long)limit) {
		fprintf(stderr, "mkrom: %s (%ld bytes) overflows slot 0x%X..0x%X\n",
			path, sz, off, limit);
		exit(1);
	}
	if (fread(img + off, 1, (size_t)sz, f) != (size_t)sz) {
		perror("fread"); exit(1);
	}
	fclose(f);
	fprintf(stderr, "  [0x%05X..0x%05lX] %s\n", off, off + sz - 1, path);
}

int main(int argc, char **argv)
{
	uint8_t *img;
	md5_t m5;
	sha1_t s1;
	uint8_t md5out[16], sha1out[20];
	FILE *o;
	int i;

	if (argc != 5) {
		fprintf(stderr,
			"usage: %s out.bin launch.bin romtags.bin minidipir.bin\n",
			argv[0]);
		return 1;
	}
	img = calloc(1, ROM_SIZE_DEFAULT);
	if (!img) { perror("calloc"); return 1; }
	place(img, argv[2], SLOT_LAUNCH,  SLOT_ROMTAGS);
	place(img, argv[3], SLOT_ROMTAGS, SLOT_DIPR);
	place(img, argv[4], SLOT_DIPR,    SLOT_FAKEHDR);
	/* SLOT_FAKEHDR..+256 stays zero: fake current OS v0.0 header. */

	o = fopen(argv[1], "wb");
	if (!o) { perror(argv[1]); return 1; }
	if (fwrite(img, 1, ROM_SIZE_DEFAULT, o) != ROM_SIZE_DEFAULT) {
		perror("fwrite"); return 1;
	}
	if (fclose(o)) { perror("fclose"); return 1; }

	crc32_init();
	md5_init(&m5); md5_update(&m5, img, ROM_SIZE_DEFAULT);
	md5_final(&m5, md5out);
	sha1_init(&s1); sha1_update(&s1, img, ROM_SIZE_DEFAULT);
	sha1_final(&s1, sha1out);

	printf("size  %u\n", (unsigned)ROM_SIZE_DEFAULT);
	printf("crc32 0x%08x\n", crc32_of(img, ROM_SIZE_DEFAULT));
	printf("md5   ");
	for (i = 0; i < 16; i++) printf("%02x", md5out[i]);
	printf("\nsha1  ");
	for (i = 0; i < 20; i++) printf("%02x", sha1out[i]);
	printf("\n");
	return 0;
}
