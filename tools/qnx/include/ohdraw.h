/* Software renderer for native Harmony 900 programs (QNX GF, RGB565).
 *
 * Frames are drawn into an off-screen GF surface (gf_surface_create, pixel
 * memory from gf_surface_get_info: stride +16, vaddr +20) and copied to the
 * display layer with gf_draw_blit2, as flash-gf does, so nothing flickers.
 * Integer-only: alpha blending, anti-aliased rounded rectangles (4x4
 * supersampled corners), vertical gradients, soft shadows, anti-aliased
 * text from tools/qnx/mkfont.py tables with per-size tracking, and a
 * critically damped spring for motion. A translation (dr_ox, dr_oy) and a
 * clip rectangle let screens be drawn at an offset for transitions.
 * Requires ohgfx.h (gfx_init) first. */
#ifndef OHDRAW_H
#define OHDRAW_H

#include <ohgfx.h>

struct ohglyph { int off; unsigned char w, h; signed char bx, by; unsigned char adv; };
struct ohfont { const unsigned char *bits; const struct ohglyph *g; int ascent, descent; };

#include "fonts/font_caption.h"
#include "fonts/font_body.h"
#include "fonts/font_small_medium.h"
#include "fonts/font_button.h"
#include "fonts/font_headline.h"
#include "fonts/font_title.h"

#define OHDRAW_LIBGF OHGFX_LIBGF ",gf_surface_create,gf_surface_get_info,gf_draw_blit2,gf_draw_finish"

#define RGB(c)	((unsigned short)((((c) >> 8) & 0xf800) | (((c) >> 5) & 0x07e0) | (((c) >> 3) & 0x001f)))

static gf_surface_t dr_surface;
static unsigned short *dr_px;
static int dr_stride;			/* in pixels */
static int dr_ox, dr_oy;		/* translation */
static int dr_cx1, dr_cy1, dr_cx2, dr_cy2;	/* clip, inclusive, screen coords */

static void dr_clip(int x1, int y1, int x2, int y2)
{
	dr_cx1 = x1 < 0 ? 0 : x1;
	dr_cy1 = y1 < 0 ? 0 : y1;
	dr_cx2 = x2 >= gfx_w ? gfx_w - 1 : x2;
	dr_cy2 = y2 >= gfx_h ? gfx_h - 1 : y2;
}

/* Returns 0 or the failed step (after gfx_init()). */
static int dr_init(void)
{
	static int info[32];
	int i;

	for (i = 0; i < 32; i++)
		info[i] = 0;
	if (gf_surface_create(&dr_surface, gfx_dev, gfx_w, gfx_h, gfx_format, NULL, 0) != 0)
		return 10;
	if (gf_surface_get_info(dr_surface, info) != 0)
		return 11;
	dr_stride = info[4] / 2;
#ifdef DR_HOST_FB
	dr_px = DR_HOST_FB;	/* rendering tests on the PC */
#else
	dr_px = (unsigned short *)info[5];
#endif
	if (!dr_px || dr_stride < gfx_w)
		return 12;
	dr_clip(0, 0, gfx_w - 1, gfx_h - 1);
	return 0;
}

/* copy a screen rectangle of the frame to the display */
static void dr_present(int x1, int y1, int x2, int y2)
{
	gf_draw_begin(gfx_ctx);
	gf_draw_blit2(gfx_ctx, dr_surface, NULL, x1, y1, x2, y2, x1, y1);
	gf_draw_finish(gfx_ctx);
	gf_draw_end(gfx_ctx);
}

/* blend 24-bit colour c over pixel d with alpha a (0..256): RGB565 spread
 * to 0x07e0f81f so that the 5-bit alpha products of the channels cannot
 * overlap */
static unsigned short dr_blend(unsigned short d, unsigned c, int a)
{
	unsigned s = RGB(c), S, D, a32 = (unsigned)(a + 4) >> 3;

	if (a32 >= 32)
		return s;
	S = (s | s << 16) & 0x07e0f81f;
	D = (d | (unsigned)d << 16) & 0x07e0f81f;
	D = ((((S - D) * a32) >> 5) + D) & 0x07e0f81f;
	return (unsigned short)(D | D >> 16);
}

/* mix two 24-bit colours, t = 0..256 */
static unsigned dr_mix(unsigned a, unsigned b, int t)
{
	unsigned r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
	unsigned g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
	unsigned bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
	return r << 16 | g << 8 | bl;
}

static void dr_hspan(int x1, int x2, int y, unsigned c, int a)
{
	unsigned short *p, s = RGB(c);

	x1 += dr_ox; x2 += dr_ox; y += dr_oy;
	if (y < dr_cy1 || y > dr_cy2)
		return;
	if (x1 < dr_cx1) x1 = dr_cx1;
	if (x2 > dr_cx2) x2 = dr_cx2;
	if (x1 > x2 || a <= 0)
		return;
	p = dr_px + y * dr_stride + x1;
	if (a >= 256)
		for (; x1 <= x2; x1++)
			*p++ = s;
	else
		for (; x1 <= x2; x1++, p++)
			*p = dr_blend(*p, c, a);
}

static void dr_pixel(int x, int y, unsigned c, int a)
{
	x += dr_ox; y += dr_oy;
	if (x < dr_cx1 || x > dr_cx2 || y < dr_cy1 || y > dr_cy2 || a <= 0)
		return;
	dr_px[y * dr_stride + x] = dr_blend(dr_px[y * dr_stride + x], c, a);
}

static void dr_fill(int x1, int y1, int x2, int y2, unsigned c, int a)
{
	for (; y1 <= y2; y1++)
		dr_hspan(x1, x2, y1, c, a);
}

static __attribute__((unused)) void dr_vgrad(int x1, int y1, int x2, int y2, unsigned top, unsigned bottom)
{
	int y, h = y2 - y1;

	for (y = y1; y <= y2; y++)
		dr_hspan(x1, x2, y, dr_mix(top, bottom, h > 0 ? (y - y1) * 256 / h : 0), 256);
}

/* corner coverage tables (0..16) for radii up to 24, built on first use */
#define DR_MAXR 24
static unsigned char dr_cov[DR_MAXR + 1][DR_MAXR * DR_MAXR];
static char dr_cov_ok[DR_MAXR + 1];

static const unsigned char *dr_corner(int r)
{
	int i, j, sx, sy, n, R = r * 8;

	if (!dr_cov_ok[r]) {
		for (j = 0; j < r; j++)
			for (i = 0; i < r; i++) {
				for (n = 0, sy = 0; sy < 4; sy++)
					for (sx = 0; sx < 4; sx++) {
						int X = i * 8 + sx * 2 + 1 - R, Y = j * 8 + sy * 2 + 1 - R;
						if (X * X + Y * Y <= R * R)
							n++;
					}
				dr_cov[r][j * r + i] = n;
			}
		dr_cov_ok[r] = 1;
	}
	return dr_cov[r];
}

/* filled rounded rectangle, anti-aliased corners, alpha a (0..256) */
static void dr_rrect(int x1, int y1, int x2, int y2, int r, unsigned c, int a)
{
	const unsigned char *cov;
	int y, i;

	if (r > DR_MAXR) r = DR_MAXR;
	if (r > (x2 - x1 + 1) / 2) r = (x2 - x1 + 1) / 2;
	if (r > (y2 - y1 + 1) / 2) r = (y2 - y1 + 1) / 2;
	if (r <= 0) {
		dr_fill(x1, y1, x2, y2, c, a);
		return;
	}
	cov = dr_corner(r);
	for (y = y1; y <= y2; y++) {
		int j = y - y1 < r ? y - y1 : y2 - y < r ? y2 - y : -1;
		if (j < 0) {
			dr_hspan(x1, x2, y, c, a);
			continue;
		}
		for (i = 0; i < r; i++) {
			int k = cov[j * r + i] * a >> 4;
			if (k >= a) {	/* rest of the row in this corner is solid */
				dr_hspan(x1 + i, x1 + r - 1, y, c, a);
				dr_hspan(x2 - r + 1, x2 - i, y, c, a);
				break;
			}
			dr_pixel(x1 + i, y, c, k);
			dr_pixel(x2 - i, y, c, k);
		}
		dr_hspan(x1 + r, x2 - r, y, c, a);
	}
}

/* soft drop shadow under a rounded rectangle */
static void dr_shadow(int x1, int y1, int x2, int y2, int r, int dy, int strength)
{
	int k;

	for (k = 4; k >= 1; k--)
		dr_rrect(x1 - k, y1 - k + dy, x2 + k, y2 + k + dy, r + k, 0x000000, strength / (k + 1));
}

/* ---- text ---- */

static int dr_glyph_index(const char **s)
{
	unsigned char c = (unsigned char)**s;
	static const unsigned char ext[] = {0x84, 0x96, 0x9c, 0xa4, 0xb6, 0xbc, 0x9f};
	int i;

	if (c == 0xc3 && (*s)[1]) {
		unsigned char d = (unsigned char)*++*s;
		for (i = 0; i < 7; i++)
			if (ext[i] == d)
				return 95 + i;
		return '?' - 32;
	}
	if (c == 0xc2 && (unsigned char)(*s)[1] == 0xb0) {
		++*s;
		return 102;
	}
	if (c < 32 || c > 126)
		return '?' - 32;
	return c - 32;
}

static int dr_text_width(const struct ohfont *f, const char *s, int track)
{
	int w = 0;

	for (; *s; s++)
		w += f->g[dr_glyph_index(&s)].adv + track;
	return w > 0 ? w - track : 0;
}

/* draw text with its baseline at y; returns the x after the last glyph */
static int dr_text(const struct ohfont *f, int x, int y, const char *s, int track, unsigned c, int a)
{
	for (; *s; s++) {
		const struct ohglyph *g = &f->g[dr_glyph_index(&s)];
		const unsigned char *b = f->bits + g->off;
		int bpr = (g->w + 1) >> 1, gx, gy;

		for (gy = 0; gy < g->h; gy++)
			for (gx = 0; gx < g->w; gx++) {
				int n = b[gy * bpr + (gx >> 1)];
				n = gx & 1 ? n & 15 : n >> 4;
				if (n)
					dr_pixel(x + g->bx + gx, y + g->by + gy,
						 c, (n * 17 + (n >> 1)) * a >> 8);
			}
		x += g->adv + track;
	}
	return x;
}

/* text cut to max_w pixels with "..." */
static void dr_text_fit(const struct ohfont *f, int x, int y, const char *s, int track,
			unsigned c, int a, int max_w)
{
	char buf[64];
	int n = 0;

	if (dr_text_width(f, s, track) <= max_w) {
		dr_text(f, x, y, s, track, c, a);
		return;
	}
	while (s[n] && n < 60) {
		buf[n] = s[n];
		buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = '.'; buf[n + 4] = 0;
		if (dr_text_width(f, buf, track) > max_w)
			break;
		n++;
	}
	buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = 0;
	dr_text(f, x, y, buf, track, c, a);
}

/* ---- motion: critically damped spring, 1/256 px, fixed 20 ms steps ---- */

struct spring { int x, v, target; };

/* response ~0.35 s: omega = 2*pi/0.35 ~ 18, k = omega^2 ~ 322, c = 2*omega ~ 36 */
static int spring_step(struct spring *s)
{
	int d = s->x - s->target;

	s->v += (-322 * d - 36 * s->v) / 50;
	s->x += s->v / 50;
	d = s->x - s->target;
	if (d > -64 && d < 64 && s->v > -512 && s->v < 512) {
		s->x = s->target;
		s->v = 0;
		return 0;	/* settled */
	}
	return 1;
}

#endif
