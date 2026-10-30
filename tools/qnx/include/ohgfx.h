/* Shared GF helpers for native Harmony 900 programs: screen setup, filled
 * rectangles and a 5x7 font (space, digits, A-Z, + - < > ^ : / . ? & ( ) _ !,
 * UTF-8 umlauts shown as their base letter), drawn at an integer scale. Include once per program; link the gf_* names
 * listed in OHGFX_LIBGF with qnxcc.py. */
#ifndef OHGFX_H
#define OHGFX_H

#include <qnx63.h>

#define OHGFX_LIBGF "gf_dev_attach,gf_dev_detach,gf_display_attach,gf_layer_attach," \
	"gf_surface_create_layer,gf_layer_set_surfaces,gf_layer_set_src_viewport," \
	"gf_layer_set_dst_viewport,gf_layer_enable,gf_layer_update,gf_context_create," \
	"gf_context_set_surface,gf_context_set_fgcolor,gf_draw_begin,gf_draw_rect,gf_draw_end"

static gf_dev_t gfx_dev;
static gf_display_t gfx_display;
static gf_context_t gfx_ctx;
static int gfx_w, gfx_h, gfx_main_layer, gfx_format;

/* Attach to the display and make the main layer ours (as flash-gf does).
 * Returns 0 or the number of the failed step. */
static int gfx_init(void)
{
	static int info[64];
	static gf_layer_t layer;
	static gf_surface_t surface;
	int i;

	for (i = 0; i < 64; i++)
		info[i] = 0;
	if (gf_dev_attach(&gfx_dev, NULL, NULL) != 0)
		return 1;
	if (gf_display_attach(&gfx_display, gfx_dev, 0, info) != 0)
		return 2;
	gfx_w = ((unsigned short *)info)[GF_DISPINFO_XRES / 2];
	gfx_h = ((unsigned short *)info)[GF_DISPINFO_YRES / 2];
	gfx_main_layer = info[GF_DISPINFO_MAIN_LAYER / 4];
	gfx_format = info[GF_DISPINFO_FORMAT / 4];
	if (gfx_w < 100 || gfx_w > 1024 || gfx_h < 100 || gfx_h > 1024)
		return 3;
	if (gf_layer_attach(&layer, gfx_display, gfx_main_layer, 0) != 0)
		return 4;
	if (gf_surface_create_layer(&surface, &layer, 1, 0, gfx_w, gfx_h,
			info[GF_DISPINFO_FORMAT / 4], NULL, 0) != 0)
		return 5;
	if (gf_context_create(&gfx_ctx) != 0 || gf_context_set_surface(gfx_ctx, surface) != 0)
		return 6;
	gf_layer_set_surfaces(layer, &surface, 1);
	gf_layer_set_src_viewport(layer, 0, 0, gfx_w - 1, gfx_h - 1);
	gf_layer_set_dst_viewport(layer, 0, 0, gfx_w - 1, gfx_h - 1);
	gf_layer_enable(layer);
	gf_layer_update(layer, 0);
	return 0;
}

static void gfx_rect(int x1, int y1, int x2, int y2, unsigned rgb)
{
	gf_context_set_fgcolor(gfx_ctx, rgb);
	gf_draw_rect(gfx_ctx, x1, y1, x2, y2);
}

/* 5x7 glyphs, one byte (low 5 bits, MSB = left) per row */
static const unsigned char gfx_digits[10][7] = {
	{14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14},
	{2,6,10,18,31,2,2}, {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
	{14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
};
static const unsigned char gfx_letters[26][7] = {
	{14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
	{31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
	{14,4,4,4,4,4,14}, {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
	{17,27,21,21,17,17,17}, {17,17,25,21,19,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
	{14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
	{17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
	{17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
};
static const struct { char c; unsigned char g[7]; } gfx_symbols[] = {
	{'+', {0,4,4,31,4,4,0}}, {'-', {0,0,0,31,0,0,0}}, {'<', {2,4,8,16,8,4,2}},
	{'>', {8,4,2,1,2,4,8}}, {'^', {4,10,17,0,0,0,0}}, {':', {0,12,12,0,12,12,0}},
	{'/', {1,1,2,4,8,16,16}}, {'.', {0,0,0,0,0,12,12}},
	{'?', {14,17,1,2,4,0,4}}, {'&', {12,18,20,8,21,18,13}}, {'(', {2,4,8,8,8,4,2}},
	{')', {8,4,2,2,2,4,8}}, {'_', {0,0,0,0,0,0,31}}, {'!', {4,4,4,4,4,0,4}},
};

static const unsigned char *gfx_glyph(int c)
{
	unsigned i;

	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (c >= '0' && c <= '9')
		return gfx_digits[c - '0'];
	if (c >= 'A' && c <= 'Z')
		return gfx_letters[c - 'A'];
	for (i = 0; i < sizeof(gfx_symbols) / sizeof(gfx_symbols[0]); i++)
		if (gfx_symbols[i].c == c)
			return gfx_symbols[i].g;
	return NULL;	/* space and unknown: blank */
}

/* UTF-8 German letters to their base letter (Ä->A, ß->S), else 0 */
static int gfx_utf8_base(unsigned char c)
{
	switch (c) {
	case 0x84: case 0xa4: return 'A';
	case 0x96: case 0xb6: return 'O';
	case 0x9c: case 0xbc: return 'U';
	case 0x9f: return 'S';
	}
	return 0;
}

/* Draw text at scale s (cell 6s x 8s); bg ~0 = transparent. Returns end x. */
static __attribute__((unused)) int gfx_text(int x, int y, const char *str, int s, unsigned fg, unsigned bg)
{
	int r, c;

	for (; *str; str++, x += 6 * s) {
		int ch = (unsigned char)*str;
		const unsigned char *g;
		if (ch == 0xc3 && str[1]) {
			ch = gfx_utf8_base((unsigned char)*++str);
			if (!ch)
				ch = '?';
		}
		g = gfx_glyph(ch);
		if (bg != ~0u)
			gfx_rect(x, y, x + 6 * s - 1, y + 8 * s - 1, bg);
		if (!g)
			continue;
		for (r = 0; r < 7; r++)
			for (c = 0; c < 5; c++)
				if (g[r] >> (4 - c) & 1)
					gfx_rect(x + c * s, y + r * s, x + c * s + s - 1, y + r * s + s - 1, fg);
	}
	return x;
}

/* n hex digits of v into buf (no terminator) */
static __attribute__((unused)) void gfx_hex(char *buf, unsigned v, int n)
{
	int k;

	for (k = 0; k < n; k++)
		buf[k] = "0123456789ABCDEF"[v >> ((n - 1 - k) * 4) & 15];
}

#endif
