/* Harmony 900: input test. Runs 120 s before the Flash UI starts.
 *
 * Key mapping: every key press (repeats ignored until the key is released)
 * adds its scancode in large hex digits, 8 per row, 32 per page; a full
 * page is cleared and the page number (right of the small row) goes up. Below that, the
 * last key event in small digits (+12 sym, +16 cap, +20 scan, +24 flags).
 * Every touch event draws a dot: white if pressed (+16 buttons != 0), grey
 * otherwise. A bar at the bottom shows the remaining time.
 *
 * Event layout (tests v2/v3): +0 type (1 touch, 2 key); touch +8 x, +12 y
 * in screen pixels, +16 buttons (4 = pressed); key +12 sym, +16 cap, +20
 * scan (bit 7 = release), +24 flags (0xE0 = scan/sym/cap valid, bit 0 =
 * down). Each press arrives as two down events. If a setup step fails,
 * its number and return value are shown in red for 15 s; a crash turns the
 * screen red with the signal number for 10 s. Built with tools/qnx/qnxcc.py. */
#include <qnx63.h>

#define RUN_MS		120000
#define POLL_MS		20
#define S		2		/* font scale: 3x5 glyphs -> 6x10 */
#define TOP		90		/* text area height */
#define NHIST		32

static gf_context_t ctx;
static int w, h;
static int drawing;	/* inside gf_draw_begin/end */

/* 3x5 hex digits, 5 rows of 3 bits each */
static const unsigned short glyph[16] = {
	075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111,
	075757, 075717, 075755, 065656, 074447, 065556, 074747, 074744,
};

static void rect(int x1, int y1, int x2, int y2, unsigned rgb)
{
	gf_context_set_fgcolor(ctx, rgb);
	gf_draw_rect(ctx, x1, y1, x2, y2);
}

static void digit(int x, int y, int d, unsigned rgb)
{
	int r, c;

	rect(x, y, x + 3 * S - 1, y + 5 * S - 1, 0x000000);
	for (r = 0; r < 5; r++)
		for (c = 0; c < 3; c++)
			if (glyph[d] >> ((4 - r) * 3 + (2 - c)) & 1)
				rect(x + c * S, y + r * S, x + c * S + S - 1, y + r * S + S - 1, rgb);
}

/* 5x7 hex digits, one byte (5 bits) per row */
static const unsigned char big[16][7] = {
	{14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14},
	{2,6,10,18,31,2,2}, {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
	{14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}, {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
	{14,17,16,16,16,17,14}, {28,18,17,17,17,18,28}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
};

static void bigdigit(int x, int y, int d, unsigned rgb)
{
	int r, c;

	rect(x, y, x + 5 * S - 1, y + 7 * S - 1, 0x000000);
	for (r = 0; r < 7; r++)
		for (c = 0; c < 5; c++)
			if (big[d][r] >> (4 - c) & 1)
				rect(x + c * S, y + r * S, x + c * S + S - 1, y + r * S + S - 1, rgb);
}

static void bighex(int x, int y, unsigned v, unsigned rgb)
{
	bigdigit(x, y, v >> 4 & 15, rgb);
	bigdigit(x + 12, y, v & 15, rgb);
}

/* n hex digits of v at x, y */
static void hex(int x, int y, unsigned v, int n, unsigned rgb)
{
	int k;

	for (k = 0; k < n; k++)
		digit(x + k * 8, y, v >> ((n - 1 - k) * 4) & 15, rgb);
}

/* label digit plus up to 4 values of 4 hex digits */
static void row(int y, int label, const unsigned *v, int n, unsigned rgb)
{
	int i;

	digit(2, y, label, 0xffffff);
	for (i = 0; i < n; i++)
		hex(14 + i * 38, y, v[i], 4, rgb);
}

static void fail(int step, int ret)
{
	digit(2, 100 + step * 12, step, 0xffffff);
	hex(14, 100 + step * 12, ret, 8, 0xff4040);
}

static void crash(int sig)
{
	if (!drawing)
		gf_draw_begin(ctx);
	rect(0, 0, w - 1, h - 1, 0xc00000);
	hex(20, 20, sig, 8, 0xffffff);
	gf_draw_end(ctx);
	sleep(10);
	_exit(1);
}

int main(int argc, char **argv, char **envp)
{
	int info[64];
	unsigned ev[16];
	gf_dev_t dev;
	gf_display_t display;
	gf_layer_t layer;
	gf_surface_t surface;
	gfi_group_t grp;
	gfi_layer_t il;
	int i, ms, ret, nhist = 0, page = 1, held = -1, step = 0;

	(void)argc; (void)argv; (void)envp;
	alarm(RUN_MS / 1000 + 10);
	for (i = 0; i < 64; i++)
		info[i] = 0;
	if (gf_dev_attach(&dev, NULL, NULL) != 0 ||
	    gf_display_attach(&display, dev, 0, info) != 0)
		return 1;
	w = ((unsigned short *)info)[GF_DISPINFO_XRES / 2];
	h = ((unsigned short *)info)[GF_DISPINFO_YRES / 2];
	if (w < 200 || w > 1024 || h < 300 || h > 1024)
		return 2;
	if (gf_layer_attach(&layer, display, info[GF_DISPINFO_MAIN_LAYER / 4], 0) != 0 ||
	    gf_surface_create_layer(&surface, &layer, 1, 0, w, h,
			info[GF_DISPINFO_FORMAT / 4], NULL, 0) != 0 ||
	    gf_context_create(&ctx) != 0 || gf_context_set_surface(ctx, surface) != 0)
		return 3;
	gf_layer_set_surfaces(layer, &surface, 1);
	gf_layer_set_src_viewport(layer, 0, 0, w - 1, h - 1);
	gf_layer_set_dst_viewport(layer, 0, 0, w - 1, h - 1);
	gf_layer_enable(layer);
	gf_layer_update(layer, 0);

	signal(SIGSEGV, crash);
	signal(SIGBUS, crash);
	signal(SIGILL, crash);

	gf_draw_begin(ctx);
	rect(0, 0, w - 1, h - 1, 0x000000);
	rect(0, TOP, w - 1, h - 9, 0x202040);
	ret = gfi_group_attach(&grp, 1, 0);
	if (ret == 0 && ++step &&
	    (ret = gfi_layer_attach(&il, grp, 0, info[GF_DISPINFO_MAIN_LAYER / 4])) == 0 && ++step &&
	    (ret = gfi_layer_set_attrs(il, 0, 0, w - 1, h - 1, 7)) == 0 && ++step)
		ret = gfi_set_keyboard_focus(grp, info[GF_DISPINFO_MAIN_LAYER / 4]);
	if (ret != 0)
		fail(step + 1, ret);
	gf_draw_end(ctx);
	if (ret != 0) {
		sleep(15);
		goto out;
	}

	for (ms = 0; ms < RUN_MS; ms += POLL_MS) {
		for (i = 0; i < 16; i++)
			ev[i] = 0;
		gf_draw_begin(ctx);
		drawing = 1;
		while ((ret = gfi_layer_get_event(&il, 1, ev, GFI_NONBLOCK)) == 0) {
			if (ev[0] == GFI_EV_POINTER) {
				int x = ev[2], y = ev[3], down = ev[4] != 0;
				if (x >= 0 && x < w && y >= 0 && y < h)
					rect(x - down - 1, y - down - 1, x + down + 1, y + down + 1,
					     down ? 0xffffff : 0x909090);
			} else if (ev[0] == GFI_EV_KEY) {
				int scan = ev[5] & 0x7f;
				row(78, 2, &ev[3], 4, 0xffff00);
				if ((ev[6] & 1) && !(ev[5] & 0x80)) {	/* down */
					if (scan != held) {
						if (nhist == NHIST) {
							rect(0, 0, w - 1, 75, 0x000000);
							nhist = 0;
							page++;
						}
						bighex(2 + (nhist % 8) * 30, 2 + (nhist / 8) * 18, scan, 0x00ff00);
						nhist++;
						bigdigit(w - 12, 76, page & 15, 0xffffff);
					}
					held = scan;
				} else if (scan == held) {
					held = -1;
				}
			} else {
				row(78, 15, ev, 4, 0xff8080);
			}
			for (i = 0; i < 16; i++)
				ev[i] = 0;
		}
		if (ret != 35 && ret != 0)
			fail(5, ret);
		i = (RUN_MS - ms) * (w - 1) / RUN_MS;	/* remaining time */
		rect(0, h - 6, i, h - 1, 0x00c000);
		rect(i + 1, h - 6, w - 1, h - 1, 0x000000);
		drawing = 0;
		gf_draw_end(ctx);
		usleep(POLL_MS * 1000);
	}
out:
	gf_dev_detach(dev);
	return 0;
}
