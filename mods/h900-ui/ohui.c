/* Harmony 900: native user interface (replaces the Flash UI).
 *
 * Reads activities and devices from data_srv (GET /userconfig) and talks to
 * hao on 127.0.0.1:1100 like the Flash UI does (XML events terminated by a
 * zero byte):
 *   StartActivity <Id>   activity cards and power off (Id -1)
 *   SendIR <ActionId>    hard keys (activity "HardButtons", or on a device
 *                        screen the device's own commands), soft buttons
 *                        ("Misc" control groups of activities and devices)
 *   SendChannel <Channel> favourite channels of an activity
 * hao's Queue/ReceiverLinkDown replies drive the status pill.
 *
 * Screens form a stack (Activities > activity, Activities > Devices >
 * device) that slides with a critically damped spring; swiping from the
 * left edge drags the screen back 1:1 and the release velocity is
 * projected to decide between going back and staying. Lists scroll 1:1
 * with momentum and rubber-banding. Touch feedback appears on touch-down,
 * a tap commits on release. Input is handled before drawing, and frames are
 * drawn at most every 33 ms unless something moves.
 *
 * "Classic UI" (end of the Activities list) ends ohui; flash.sh then starts
 * the Flash player, as it does if ohui fails. Log: /usr/local/cache/ohui.log. */
#include <ohdraw.h>

#define POLL_MS		15
#define FRAME_MS	33
#define REPEAT_DELAY	400
#define REPEAT_MS	150
#define SLOP		10
#define EDGE		22
#define MAXACT		6
#define MAXHARD		48
#define MAXSOFT		12
#define MAXCHAN		12
#define MAXDEV		8
#define MAXCMD		72

/* colours (dark appearance) */
#define C_BG		0x000000
#define C_CARD		0x1c1c1e
#define C_CARD_HI	0x3a3a3c
#define C_TILE		0x262628
#define C_TILE_HI	0x48484a
#define C_TEXT		0xffffff
#define C_TEXT2		0x8e8e93
#define C_TEXT3		0x636366
#define C_ACCENT	0x0a84ff
#define C_GREEN		0x30d158
#define C_YELLOW	0xffd60a
#define C_RED		0xff453a
#define C_RED_BG	0x2c1412
#define C_RED_BG_HI	0x4a221d

static const unsigned tints[] = { 0x0a84ff, 0xbf5af2, 0xff9f0a, 0x64d2ff, 0x30d158, 0xff375f };

struct hard { char name[16]; char action[48]; };
struct cmd { char label[24]; char action[48]; int pos; };
struct chan { char station[20]; char number[8]; };
struct activity {
	char id[16], label[24];
	struct hard hard[MAXHARD];
	int nhard;
	struct cmd soft[MAXSOFT];
	int nsoft;
	struct chan chan[MAXCHAN];
	int nchan;
};
struct device {
	char id[16], label[24], type[20];
	struct cmd cmd[MAXCMD];
	int ncmd;
};

static char cfg[192 * 1024];
static struct activity act[MAXACT];
static int nact;
static struct device dev[MAXDEV];
static int ndev;
static int running = -1;	/* activity hao last started */
static int hao = -1;
static char rx[8192];
static int rxlen;
static char logbuf[24 * 1024];
static int loglen;
static char clock_text[8] = "";
static int battery = -1;	/* level 0..8 (cache/batt_lvls.dat: mV thresholds for 1..8) */

static char status_text[24] = "Ready";
static unsigned status_color = C_TEXT2;

/* ---- screens ---- */

enum { S_HOME, S_ACT, S_DEVICES, S_DEVICE };
struct screen {
	int kind, index;
	int sy, sv;		/* scroll offset and velocity, 1/256 px and 1/256 px/s */
	int content_h;		/* set when drawn */
};
static struct screen stack[4];
static int depth;		/* index of the top screen */
static struct spring nav;	/* depth * gfx_w * 256 when settled */

/* touch */
enum { T_NONE, T_ACT, T_POWER, T_CLASSIC, T_BACK, T_SOFT, T_CHAN, T_DEVICES, T_DEV, T_CMD };
enum { G_NONE, G_TAP, G_SCROLL, G_EDGE };
static int press_kind = T_NONE, press_index;
static int gesture = G_NONE;
static int g_x0, g_y0, g_sy0;
static int g_lx, g_ly, g_vx, g_vy;	/* last point and velocity, px/s */
static long g_lt;

/* scancode -> Logitech button name */
static const struct { unsigned char scan; const char *name; char repeat; } keymap[] = {
	{0x1A, "Menu", 0}, {0x21, "Exit", 0}, {0x10, "Info", 0}, {0x32, "Guide", 0},
	{0x16, "Red", 0}, {0x17, "Green", 0}, {0x1F, "Yellow", 0}, {0x27, "Blue", 0},
	{0x48, "DirectionUp", 1}, {0x50, "DirectionDown", 1}, {0x4B, "DirectionLeft", 1},
	{0x4D, "DirectionRight", 1}, {0x4C, "Select", 0},
	{0x0B, "ChannelUp", 1}, {0x19, "ChannelDown", 1}, {0x26, "PrevChannel", 0},
	{0x0A, "VolumeUp", 1}, {0x18, "VolumeDown", 1}, {0x25, "VolumeMute", 0},
	{0x2E, "Rewind", 0}, {0x2F, "Play", 0}, {0x01, "FastForward", 0},
	{0x04, "SkipBackward", 0}, {0x20, "SkipForward", 0},
	{0x13, "Record", 0}, {0x14, "Pause", 0}, {0x1E, "Stop", 0},
	{0x2B, "Number1", 0}, {0x2D, "Number2", 0}, {0x2C, "Number3", 0},
	{0x39, "Number4", 0}, {0x29, "Number5", 0}, {0x31, "Number6", 0},
	{0x11, "Number7", 0}, {0x35, "Number8", 0}, {0x33, "Number9", 0},
	{0x30, "Number0", 0}, {0x08, "NumberEnter", 0}, {0x28, "Clear", 0},
	{0x53, "PageUp", 1}, {0x0E, "PageDown", 1},
};
#define NKEYMAP ((int)(sizeof(keymap) / sizeof(keymap[0])))
/* device command names differ from the activity button names */
static const struct { const char *from, *to; } aliases[] = {
	{"Select", "OK"}, {"VolumeMute", "Mute"}, {"Number0", "0"}, {"Number1", "1"},
	{"Number2", "2"}, {"Number3", "3"}, {"Number4", "4"}, {"Number5", "5"},
	{"Number6", "6"}, {"Number7", "7"}, {"Number8", "8"}, {"Number9", "9"},
	{"NumberEnter", "Enter"}, {"PrevChannel", "Back"},
};
#define SCAN_ACTIVITIES	0x41
#define SCAN_OFF	0x22
#define SCAN_UNDER_L	0x1B

/* ---- small string helpers ---- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int seq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static const char *find(const char *s, const char *end, const char *pat)
{
	int n = slen(pat), i;

	for (; s + n <= end; s++) {
		for (i = 0; i < n && s[i] == pat[i]; i++)
			;
		if (i == n)
			return s;
	}
	return NULL;
}

static void copy_tag(char *dst, int max, const char *s, const char *end,
		     const char *tag, const char *etag)
{
	const char *a = find(s, end, tag), *b;
	int k = 0;

	dst[0] = 0;
	if (!a)
		return;
	a += slen(tag);
	b = find(a, end, etag);
	if (!b)
		return;
	while (a < b && k < max - 1)
		dst[k++] = *a++;
	dst[k] = 0;
}

static int cat(char *dst, int k, const char *s)
{
	while (*s)
		dst[k++] = *s++;
	dst[k] = 0;
	return k;
}

static void scopy(char *dst, const char *s, int max)
{
	int k = 0;

	while (*s && k < max - 1)
		dst[k++] = *s++;
	dst[k] = 0;
}

static int catnum(char *dst, int k, int v)
{
	char t[12];
	int n = 0;

	if (v < 0) {
		dst[k++] = '-';
		v = -v;
	}
	do
		t[n++] = '0' + v % 10;
	while ((v /= 10) && n < 11);
	while (n)
		dst[k++] = t[--n];
	dst[k] = 0;
	return k;
}

#ifndef OHUI_HOST
static void logs(const char *a, const char *b)
{
	int fd, n = slen(a) + (b ? slen(b) : 0);

	if (loglen + n + 2 > (int)sizeof(logbuf))
		return;
	loglen = cat(logbuf, loglen, a);
	if (b)
		loglen = cat(logbuf, loglen, b);
	logbuf[loglen++] = '\n';
	fd = creat("/usr/local/cache/ohui.log", 0644);
	if (fd >= 0) {
		write(fd, logbuf, loglen);
		close(fd);
	}
}

static void lognum(const char *a, int v)
{
	char b[16];

	catnum(b, 0, v);
	logs(a, b);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(0, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#else
static void logs(const char *a, const char *b) { (void)a; (void)b; }
#endif

/* ---- config ---- */

/* buttons of the "Misc" control group, or of all groups (group NULL) */
static void parse_cmds(struct cmd *c, int *n, int max, const char *s, const char *end,
		       const char *group)
{
	const char *g = group ? find(s, end, group) : s, *ge, *b, *be;
	int k;

	ge = !g ? NULL : group ? find(g, end, "</ControlGroup>") : end;
	for (b = g; ge && *n < max && (b = find(b, ge, "<Button>")); b = be) {
		struct cmd *x = &c[*n];
		char pos[8];
		be = find(b, ge, "</Button>");
		if (!be)
			break;
		copy_tag(x->label, sizeof(x->label), b, be, "<Label>", "</Label>");
		if (!x->label[0]) {	/* no label: icon name, "teletext_on" -> "Teletext on" */
			copy_tag(x->label, sizeof(x->label), b, be, "<Icon>", "</Icon>");
			for (k = 0; x->label[k]; k++)
				if (x->label[k] == '_')
					x->label[k] = ' ';
			if (x->label[0] >= 'a' && x->label[0] <= 'z')
				x->label[0] -= 'a' - 'A';
		}
		for (k = 0; x->label[k]; k++)	/* collapse double spaces */
			if (x->label[k] == ' ' && x->label[k + 1] == ' ') {
				int j;
				for (j = k; x->label[j]; j++)
					x->label[j] = x->label[j + 1];
				k--;
			}
		copy_tag(x->action, sizeof(x->action), b, be, "<ActionId>", "</ActionId>");
		copy_tag(pos, sizeof(pos), b, be, "<Position>", "</Position>");
		x->pos = 0;
		for (k = 0; pos[k] >= '0' && pos[k] <= '9'; k++)
			x->pos = x->pos * 10 + pos[k] - '0';
		if (x->action[0])
			(*n)++;
	}
	for (k = 1; k < *n; k++) {	/* sort by position */
		int j = k;
		while (j > 0 && c[j - 1].pos > c[j].pos) {
			struct cmd t = c[j];
			c[j] = c[j - 1];
			c[j - 1] = t;
			j--;
		}
	}
}

static void parse_activity(struct activity *a, const char *s, const char *end)
{
	const char *g, *ge, *b, *be, *q;
	int k;

	copy_tag(a->id, sizeof(a->id), s, end, "<Id>", "</Id>");
	copy_tag(a->label, sizeof(a->label), s, end, "<Label>", "</Label>");
	g = find(s, end, "<ControlGroup name=\"HardButtons\">");
	ge = g ? find(g, end, "</ControlGroup>") : NULL;
	for (b = g; ge && a->nhard < MAXHARD && (b = find(b, ge, "<Button name=\"")); b = be) {
		struct hard *h = &a->hard[a->nhard];
		be = find(b, ge, "</Button>");
		if (!be)
			break;
		b += 14;
		for (k = 0, q = b; *q != '"' && k < (int)sizeof(h->name) - 1; q++)
			h->name[k++] = *q;
		h->name[k] = 0;
		copy_tag(h->action, sizeof(h->action), b, be, "<ActionId>", "</ActionId>");
		if (h->action[0])
			a->nhard++;
	}
	parse_cmds(a->soft, &a->nsoft, MAXSOFT, s, end, "<ControlGroup name=\"Misc\">");
	g = find(s, end, "<ChannelList>");
	ge = g ? find(g, end, "</ChannelList>") : NULL;
	for (b = g; ge && a->nchan < MAXCHAN && (b = find(b, ge, "<Channel>")); b = be) {
		struct chan *c = &a->chan[a->nchan];
		be = find(b, ge, "</Channel>");
		if (!be)
			break;
		copy_tag(c->station, sizeof(c->station), b, be, "<Station>", "</Station>");
		copy_tag(c->number, sizeof(c->number), b, be, "<Number>", "</Number>");
		if (c->number[0])
			a->nchan++;
	}
}

/* "ChannelDown" -> "Channel Down", "InputHdmi1" -> "Input Hdmi 1" */
static void pretty(char *dst, int max, const char *name)
{
	int k = 0, i;

	for (i = 0; name[i] && k < max - 2; i++) {
		char c = name[i], p = i ? name[i - 1] : 0;
		if (i && ((c >= 'A' && c <= 'Z' && p >= 'a' && p <= 'z') ||
			  (c >= '0' && c <= '9' && !(p >= '0' && p <= '9'))))
			dst[k++] = ' ';
		dst[k++] = c;
	}
	dst[k] = 0;
}

static void parse_device(struct device *d, const char *s, const char *end)
{
	const char *p, *c, *ce;
	int k;

	copy_tag(d->id, sizeof(d->id), s, end, "<Id>", "</Id>");
	copy_tag(d->type, sizeof(d->type), s, end, "<Type>", "</Type>");
	p = find(s, end, "<Presentation>");
	copy_tag(d->label, sizeof(d->label), p ? p : s, end, "<Label>", "</Label>");
	/* labelled buttons first (Presentation, all control groups) */
	ce = p ? find(p, end, "</Presentation>") : NULL;
	if (p && ce)
		parse_cmds(d->cmd, &d->ncmd, MAXCMD, p, ce, NULL);
	/* then every other command of the device: ActionId <device>_<Name>_Hold */
	for (c = find(s, end, "<Commands>"); c && d->ncmd < MAXCMD && (c = find(c, end, "<Command><Name>")); c = ce) {
		struct cmd *x = &d->cmd[d->ncmd];
		char name[32];
		ce = find(c, end, "</Command>");
		if (!ce)
			break;
		copy_tag(name, sizeof(name), c, ce, "<Name>", "</Name>");
		k = cat(x->action, 0, d->id);
		k = cat(x->action, k, "_");
		k = cat(x->action, k, name);
		cat(x->action, k, "_Hold");
		for (k = 0; k < d->ncmd && !seq(d->cmd[k].action, x->action); k++)
			;
		if (k < d->ncmd || !name[0])
			continue;	/* already there with its label */
		pretty(x->label, sizeof(x->label), name);
		x->pos = 1000 + d->ncmd;
		d->ncmd++;
	}
}

static void parse_config(const char *s, int n)
{
	const char *end = s + n, *a, *e, *p;
	int i;

	for (p = s; nact < MAXACT && (a = find(p, end, "<Activity>")) && (e = find(a, end, "</Activity>")); p = e) {
		parse_activity(&act[nact], a, e);
		if (act[nact].id[0])
			nact++;
	}
	for (p = s; ndev < MAXDEV && (a = find(p, end, "<Device>")) && (e = find(a, end, "</Device>")); p = e) {
		parse_device(&dev[ndev], a, e);
		if (dev[ndev].id[0] && dev[ndev].ncmd)
			ndev++;
	}
	for (i = 0; i < nact - 1; i++)	/* power off (Id -1) last */
		if (act[i].id[0] == '-') {
			struct activity t = act[i];
			int j;
			for (j = i; j < nact - 1; j++)
				act[j] = act[j + 1];
			act[nact - 1] = t;
			break;
		}
}

static int power_off_index(void)
{
	int i;

	for (i = 0; i < nact; i++)
		if (act[i].id[0] == '-')
			return i;
	return -1;
}

static int n_activities(void)
{
	return power_off_index() >= 0 ? nact - 1 : nact;
}

static unsigned device_tint(const struct device *d)
{
	if (seq(d->type, "Television")) return 0x0a84ff;
	if (seq(d->type, "Pvr") || seq(d->type, "SatelliteBox")) return 0xbf5af2;
	if (seq(d->type, "Light")) return 0xffd60a;
	if (seq(d->type, "DvdCdRadio") || seq(d->type, "Amplifier")) return 0xff9f0a;
	return 0x64d2ff;
}

/* ---- layout (240 x 320); content coordinates start below the bars ---- */

#define STATUS_H	20
#define NAV_H		28
#define M		12
#define POWER_Y		(gfx_h - 50)
#define POWER_H		40
#define CARD_H		54
#define CARD_STEP	62
#define TILE_W		((gfx_w - 2 * M - 8) / 2)
#define TILE_H		38
#define TILE_STEP	44

static int has_power_bar(const struct screen *s) { return s->kind <= S_ACT && power_off_index() >= 0; }
static int has_nav(const struct screen *s) { return s->kind != S_HOME; }
static int content_top(const struct screen *s) { return STATUS_H + (has_nav(s) ? NAV_H : 0); }
static int content_bottom(const struct screen *s) { return has_power_bar(s) ? POWER_Y - 6 : gfx_h - 1; }

/* home content */
#define H_TITLE		36
#define H_CARD(i)	(52 + (i) * CARD_STEP)
/* activity / device content */
#define A_TITLE		30
#define A_SECTION1	46
#define A_TILE_Y(i)	(56 + ((i) >> 1) * TILE_STEP)
#define TILE_X(i)	(M + ((i) & 1) * (TILE_W + 8))

static int tiles_end(int n) { return n ? A_TILE_Y(n - 1) + TILE_H : 40; }

static int scroll_max(const struct screen *s)
{
	int h = content_bottom(s) - content_top(s) + 1, m = s->content_h - h;
	return m > 0 ? m : 0;
}

static int in(int x, int y, int x1, int y1, int x2, int y2, int slop)
{
	return x >= x1 - slop && x <= x2 + slop && y >= y1 - slop && y <= y2 + slop;
}

/* control under screen point (x, y) on the top screen */
static void hit(int x, int y, int slop, int *kind, int *index)
{
	struct screen *s = &stack[depth];
	int i, cy, n;

	*kind = T_NONE;
	*index = 0;
	if (has_power_bar(s) && in(x, y, M, POWER_Y, gfx_w - M, POWER_Y + POWER_H, slop)) {
		*kind = T_POWER;
		return;
	}
	if (has_nav(s) && y >= STATUS_H - slop && y < STATUS_H + NAV_H + slop) {
		if (x < 140)
			*kind = T_BACK;
		return;
	}
	if (y < content_top(s) || y > content_bottom(s))
		return;
	cy = y - content_top(s) + (s->sy >> 8);
	switch (s->kind) {
	case S_HOME:
		n = n_activities();
		for (i = 0; i <= n; i++)
			if (in(x, cy, M, H_CARD(i), gfx_w - M, H_CARD(i) + CARD_H - 1, slop)) {
				*kind = i < n ? T_ACT : T_DEVICES;
				*index = i;
				return;
			}
		if (in(x, cy, M, H_CARD(n + 1), gfx_w - M, H_CARD(n + 1) + 40, slop))
			*kind = T_CLASSIC;
		return;
	case S_ACT: {
		struct activity *a = &act[s->index];
		int ch_y = tiles_end(a->nsoft) + 34;
		for (i = 0; i < a->nsoft; i++)
			if (in(x, cy, TILE_X(i), A_TILE_Y(i), TILE_X(i) + TILE_W - 1, A_TILE_Y(i) + TILE_H - 1, slop)) {
				*kind = T_SOFT;
				*index = i;
				return;
			}
		for (i = 0; i < a->nchan; i++) {
			int ty = ch_y + (i >> 1) * TILE_STEP;
			if (in(x, cy, TILE_X(i), ty, TILE_X(i) + TILE_W - 1, ty + TILE_H - 1, slop)) {
				*kind = T_CHAN;
				*index = i;
				return;
			}
		}
		return;
	}
	case S_DEVICES:
		for (i = 0; i < ndev; i++)
			if (in(x, cy, M, A_SECTION1 + 4 + i * CARD_STEP, gfx_w - M,
			       A_SECTION1 + 4 + i * CARD_STEP + CARD_H - 1, slop)) {
				*kind = T_DEV;
				*index = i;
				return;
			}
		return;
	case S_DEVICE: {
		struct device *d = &dev[s->index];
		for (i = 0; i < d->ncmd; i++)
			if (in(x, cy, TILE_X(i), A_TILE_Y(i), TILE_X(i) + TILE_W - 1, A_TILE_Y(i) + TILE_H - 1, slop)) {
				*kind = T_CMD;
				*index = i;
				return;
			}
		return;
	}
	}
}

/* ---- drawing ---- */

static int pressed(int kind, int index)
{
	return press_kind == kind && press_index == index && gesture == G_TAP;
}

static void draw_status_bar(void)
{
	int x;

	dr_fill(0, 0, gfx_w - 1, STATUS_H - 1, C_BG, 256);
	dr_text(&font_small_medium, M, 14, clock_text, 0, C_TEXT, 256);
	if (battery >= 0) {
		int fillw = 18 * battery / 8;
		unsigned c = battery <= 1 ? C_RED : battery <= 2 ? C_YELLOW : C_TEXT;
		x = gfx_w - M - 24;
		dr_rrect(x, 5, x + 21, 15, 3, C_TEXT, 100);		/* battery body */
		dr_rrect(x + 1, 6, x + 20, 14, 2, C_BG, 256);
		if (fillw > 0)
			dr_rrect(x + 2, 7, x + 1 + fillw, 13, 2, c, 256);
		dr_fill(x + 22, 8, x + 23, 12, C_TEXT, 100);		/* terminal */
	}
}

static void draw_power_bar(void)
{
	const char *t = "Power Off";
	int w = dr_text_width(&font_button, t, 0), y;

	/* scroll edge: content fades out above the bar */
	for (y = POWER_Y - 16; y < POWER_Y - 6; y++)
		dr_hspan(0, gfx_w - 1, y, C_BG, (y - (POWER_Y - 16)) * 26);
	dr_fill(0, POWER_Y - 6, gfx_w - 1, gfx_h - 1, C_BG, 256);
	dr_rrect(M, POWER_Y, gfx_w - 1 - M, POWER_Y + POWER_H - 1, 12,
		 pressed(T_POWER, 0) ? C_RED_BG_HI : C_RED_BG, 256);
	dr_text(&font_button, (gfx_w - w) / 2, POWER_Y + 26, t, 0, C_RED, 256);
}

static const char *back_label(const struct screen *s)
{
	return s->kind == S_DEVICE ? "Devices" : "Activities";
}

static void draw_nav(const struct screen *s, const char *title)
{
	int a = pressed(T_BACK, 0) ? 110 : 256, w;

	dr_fill(0, STATUS_H, gfx_w - 1, STATUS_H + NAV_H - 1, C_BG, 256);
	for (w = 0; w < 6; w++) {	/* back chevron "<" */
		dr_fill(M + w, STATUS_H + 13 - w, M + w + 1, STATUS_H + 14 - w, C_ACCENT, a);
		dr_fill(M + w, STATUS_H + 13 + w, M + w + 1, STATUS_H + 14 + w, C_ACCENT, a);
	}
	dr_text(&font_body, M + 12, STATUS_H + 19, back_label(s), 0, C_ACCENT, a);
	/* collapsed title once the large title has scrolled away */
	if (title && (s->sy >> 8) > A_TITLE) {
		int t = (s->sy >> 8) - A_TITLE, al = t > 16 ? 256 : t * 16;
		w = dr_text_width(&font_small_medium, title, 0);
		if (w < gfx_w - 2 * 110 + 40 && !(s->kind == S_ACT || s->kind == S_DEVICE))
			dr_text(&font_small_medium, (gfx_w - w) / 2, STATUS_H + 18, title, 0, C_TEXT, al);
		dr_hspan(0, gfx_w - 1, STATUS_H + NAV_H - 1, 0xffffff, al / 10);
	}
	if (s->kind == S_ACT || s->kind == S_DEVICE) {	/* status pill */
		w = dr_text_width(&font_caption, status_text, 0) + 22;
		if (w < 104) {
			dr_rrect(gfx_w - M - w, STATUS_H + 5, gfx_w - M - 1, STATUS_H + 23, 9, status_color, 46);
			dr_rrect(gfx_w - M - w + 8, STATUS_H + 11, gfx_w - M - w + 13, STATUS_H + 16, 3, status_color, 256);
			dr_text(&font_caption, gfx_w - M - w + 17, STATUS_H + 18, status_text, 0, status_color, 256);
		}
	}
}

static void section(int y, const char *t)
{
	char up[32];
	int k;

	for (k = 0; t[k] && k < 31; k++)
		up[k] = t[k] >= 'a' && t[k] <= 'z' ? t[k] - ('a' - 'A') : t[k];
	up[k] = 0;
	dr_text(&font_caption, M + 4, y, up, 1, C_TEXT2, 256);
}

static void tile(int x, int y, const char *label, const char *num, int hi)
{
	int w, inset = hi ? 1 : 0;

	dr_rrect(x + inset, y + inset, x + TILE_W - 1 - inset, y + TILE_H - 1 - inset, 11,
		 hi ? C_TILE_HI : C_TILE, 256);
	dr_hspan(x + 11, x + TILE_W - 12, y + inset, 0xffffff, 14);	/* light on the top edge */
	if (num) {	/* channel: number in accent, station after it */
		int nx = dr_text(&font_button, x + 9, y + 25, num, 0, C_ACCENT, 256);
		dr_text_fit(&font_body, nx + 6, y + 24, label, 0, C_TEXT, 256, x + TILE_W - 8 - nx - 6);
		return;
	}
	w = dr_text_width(&font_body, label, 0);
	if (w > TILE_W - 12)
		dr_text_fit(&font_body, x + 6, y + 24, label, 0, C_TEXT, 256, TILE_W - 12);
	else
		dr_text(&font_body, x + (TILE_W - w) / 2, y + 24, label, 0, C_TEXT, 256);
}

static void card(int y, unsigned tint, const char *initial, const char *title, const char *sub,
		 unsigned sub_c, int dot, int hi)
{
	int inset = hi ? 2 : 0, k, cx = gfx_w - M - 16, cyy = y + 27;

	dr_rrect(M + inset, y + inset, gfx_w - 1 - M - inset, y + CARD_H - 1 - inset, 14,
		 hi ? C_CARD_HI : C_CARD, 256);
	dr_hspan(M + 14, gfx_w - 1 - M - 14, y + inset, 0xffffff, 18);
	dr_rrect(M + 10, y + 10, M + 43, y + 43, 17, tint, 256);
	dr_text(&font_headline, M + 27 - dr_text_width(&font_headline, initial, 0) / 2, y + 33,
		initial, 0, C_TEXT, 256);
	dr_text_fit(&font_button, M + 54, y + 24, title, 0, C_TEXT, 256, gfx_w - 2 * M - 80);
	if (dot)
		dr_rrect(M + 54, y + 34, M + 59, y + 39, 3, sub_c, 256);
	dr_text(&font_caption, M + 54 + (dot ? 9 : 0), y + 41, sub, 0, sub_c, 256);
	for (k = 0; k < 5; k++) {	/* chevron */
		dr_fill(cx + k, cyy - 5 + k, cx + k + 1, cyy - 4 + k, C_TEXT3, 256);
		dr_fill(cx + k, cyy + 5 - k, cx + k + 1, cyy + 6 - k, C_TEXT3, 256);
	}
}

static void initial_of(char *dst, const char *label)
{
	dst[0] = label[0] ? label[0] : '?';
	dst[1] = 0;
	if ((unsigned char)dst[0] == 0xc3) {
		dst[1] = label[1];
		dst[2] = 0;
	} else if (dst[0] >= 'a' && dst[0] <= 'z') {
		dst[0] -= 'a' - 'A';
	}
}

/* draw screen s with its content scrolled; dr_ox is the horizontal offset */
static void draw_screen(struct screen *s)
{
	int top = content_top(s), i, y;
	int cx1 = dr_cx1, cx2 = dr_cx2;		/* horizontal clip set by the caller */
	char ini[4];

	dr_fill(0, 0, gfx_w - 1, gfx_h - 1, C_BG, 256);
	dr_clip(cx1, top, cx2, content_bottom(s));
	dr_oy = top - (s->sy >> 8);
	switch (s->kind) {
	case S_HOME: {
		int n = n_activities();
		dr_text(&font_title, M, H_TITLE, "Activities", -1, C_TEXT, 256);
		for (i = 0; i < n; i++) {
			initial_of(ini, act[i].label);
			card(H_CARD(i), tints[i % 6], ini, act[i].label,
			     i == running ? "Running" : "Tap to start",
			     i == running ? C_GREEN : C_TEXT2, i == running, pressed(T_ACT, i));
		}
		{
			char sub[24];
			cat(sub, catnum(sub, 0, ndev), ndev == 1 ? " device" : " devices");
			card(H_CARD(n), 0x48484a, "D", "Devices", sub, C_TEXT2, 0, pressed(T_DEVICES, n));
		}
		y = H_CARD(n + 1);
		{
			const char *t = "Classic UI";
			int w = dr_text_width(&font_body, t, 0);
			dr_text(&font_body, (gfx_w - w) / 2, y + 24, t, 0,
				pressed(T_CLASSIC, 0) ? C_TEXT : C_TEXT2, 256);
		}
		s->content_h = y + 44;
		break;
	}
	case S_ACT: {
		struct activity *a = &act[s->index];
		dr_text_fit(&font_title, M, A_TITLE, a->label, -1, C_TEXT, 256, gfx_w - 2 * M);
		if (a->nsoft)
			section(A_SECTION1, "Controls");
		for (i = 0; i < a->nsoft; i++)
			tile(TILE_X(i), A_TILE_Y(i), a->soft[i].label, NULL, pressed(T_SOFT, i));
		y = tiles_end(a->nsoft) + 34;
		if (a->nchan) {
			section(y - 10, "Favorites");
			for (i = 0; i < a->nchan; i++)
				tile(TILE_X(i), y + (i >> 1) * TILE_STEP, a->chan[i].station, a->chan[i].number,
				     pressed(T_CHAN, i));
			y += ((a->nchan + 1) >> 1) * TILE_STEP;
		}
		if (!a->nsoft && !a->nchan)
			dr_text(&font_body, M, 70, "Use the keys to control it.", 0, C_TEXT2, 256);
		s->content_h = y + 8;
		break;
	}
	case S_DEVICES:
		dr_text(&font_title, M, A_TITLE, "Devices", -1, C_TEXT, 256);
		for (i = 0; i < ndev; i++) {
			char sub[32];
			cat(sub, catnum(sub, 0, dev[i].ncmd), " commands");
			initial_of(ini, dev[i].label);
			card(A_SECTION1 + 4 + i * CARD_STEP, device_tint(&dev[i]), ini, dev[i].label, sub,
			     C_TEXT2, 0, pressed(T_DEV, i));
		}
		s->content_h = A_SECTION1 + 4 + ndev * CARD_STEP + 8;
		break;
	case S_DEVICE: {
		struct device *d = &dev[s->index];
		dr_text_fit(&font_title, M, A_TITLE, d->label, -1, C_TEXT, 256, gfx_w - 2 * M);
		section(A_SECTION1, d->type);
		for (i = 0; i < d->ncmd; i++)
			tile(TILE_X(i), A_TILE_Y(i), d->cmd[i].label, NULL, pressed(T_CMD, i));
		s->content_h = tiles_end(d->ncmd) + 12;
		break;
	}
	}
	dr_oy = 0;
	dr_clip(cx1, 0, cx2, gfx_h - 1);
	if (s->sv || gesture == G_SCROLL) {	/* scroll indicator while moving */
		int vis = content_bottom(s) - top, mx = scroll_max(s);
		if (mx > 0) {
			int h = vis * vis / s->content_h, pos = s->sy >> 8;
			if (h < 20) h = 20;
			if (pos < 0) pos = 0;
			if (pos > mx) pos = mx;
			y = top + 2 + (vis - h - 4) * pos / mx;
			dr_rrect(gfx_w - 5, y, gfx_w - 3, y + h, 1, 0xffffff, 90);
		}
	}
	if (has_nav(s))
		draw_nav(s, s->kind == S_DEVICES ? "Devices" : NULL);
	if (has_power_bar(s))
		draw_power_bar();
}

/* full frame for the current navigation position */
static void render(void)
{
	int p = nav.x >> 8, base = p / gfx_w, frac = p - base * gfx_w;

	if (p < 0) { base = 0; frac = 0; }
	if (base > depth) { base = depth; frac = 0; }
	dr_ox = 0;
	dr_clip(0, 0, gfx_w - 1, gfx_h - 1);
	if (frac == 0 || base + 1 > depth) {
		draw_screen(&stack[base]);
	} else {
		/* lower screen moves left at a third of the speed and dims;
		 * the upper one slides over it with a soft edge shadow */
		int cut = gfx_w - frac, k;
		dr_ox = -frac / 3;
		dr_clip(0, 0, cut - 1, gfx_h - 1);
		draw_screen(&stack[base]);
		dr_ox = 0;
		dr_clip(0, 0, cut - 1, gfx_h - 1);
		dr_fill(0, 0, cut - 1, gfx_h - 1, 0x000000, frac * 110 / gfx_w);
		for (k = 1; k <= 6; k++)
			dr_fill(cut - k, 0, cut - k, gfx_h - 1, 0x000000, 70 - k * 10);
		dr_ox = cut;
		dr_clip(cut, 0, gfx_w - 1, gfx_h - 1);
		draw_screen(&stack[base + 1]);
	}
	dr_ox = 0;
	dr_clip(0, 0, gfx_w - 1, gfx_h - 1);
	draw_status_bar();
}

/* ---- navigation ---- */

static void push(int kind, int index)
{
	if (depth >= 3)
		return;
	depth++;
	stack[depth].kind = kind;
	stack[depth].index = index;
	stack[depth].sy = stack[depth].sv = 0;
	stack[depth].content_h = 0;
	nav.target = depth * gfx_w << 8;
}

static void pop_to(int d)
{
	if (d < 0)
		d = 0;
	nav.target = d * gfx_w << 8;	/* depth follows when the spring settles */
}

/* drop screens that have slid away */
static void settle_stack(void)
{
	int d = nav.target / (gfx_w << 8);

	if (nav.x == nav.target && d < depth)
		depth = d;
}

/* scroll physics for a screen; returns 1 while moving */
static int scroll_step(struct screen *s)
{
	int mx = scroll_max(s) << 8;

	if (gesture == G_SCROLL)
		return 0;
	if (s->sy < 0 || s->sy > mx) {		/* out of bounds: spring back */
		struct spring sp;
		sp.x = s->sy;
		sp.v = s->sv;
		sp.target = s->sy < 0 ? 0 : mx;
		spring_step(&sp);
		s->sy = sp.x;
		s->sv = sp.v;
		return 1;
	}
	if (!s->sv)
		return 0;
	s->sy += s->sv / 50;			/* coast */
	s->sv = s->sv * 24 / 25;
	if (s->sv > -2560 && s->sv < 2560)	/* below 10 px/s */
		s->sv = 0;
	return 1;
}

/* rubber band for a drag beyond the edge (px) */
static int rubber(int over, int dim)
{
	int a = over < 0 ? -over : over;

	return over * dim * 55 / (100 * dim + 55 * a);
}

#ifndef OHUI_HOST
/* ---- network ---- */

static int tcp(const char *port)
{
	struct addrinfo hints, *res = NULL;
	int fd = -1, i;

	for (i = 0; i < (int)sizeof(hints); i++)
		((char *)&hints)[i] = 0;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo("127.0.0.1", port, &hints, &res) != 0 || !res)
		return -1;
	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int readable(int fd, int ms)
{
	unsigned set[8];
	struct timeval tv;
	int i;

	for (i = 0; i < 8; i++)
		set[i] = 0;
	set[fd >> 5] = 1u << (fd & 31);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select(fd + 1, set, NULL, NULL, &tv) > 0;
}

static const char *http_get(const char *path, char *buf, int size, int *len)
{
	int fd = tcp("80"), n = 0, r, want = -1, k = 0;
	char req[128];
	const char *hdr_end, *cl;

	if (fd < 0)
		return NULL;
	k = cat(req, k, "GET ");
	k = cat(req, k, path);
	k = cat(req, k, " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
	write(fd, req, k);
	while (n < size - 1 && readable(fd, 2000)) {
		r = read(fd, buf + n, size - 1 - n);
		if (r <= 0)
			break;
		n += r;
		hdr_end = find(buf, buf + n, "\r\n\r\n");
		if (hdr_end && want < 0 && (cl = find(buf, hdr_end, "Content-Length: "))) {
			want = 0;
			for (cl += 16; *cl >= '0' && *cl <= '9'; cl++)
				want = want * 10 + *cl - '0';
		}
		if (hdr_end && want >= 0 && n - (hdr_end + 4 - buf) >= want)
			break;
	}
	close(fd);
	buf[n] = 0;
	hdr_end = find(buf, buf + n, "\r\n\r\n");
	if (!hdr_end)
		return NULL;
	*len = n - (hdr_end + 4 - buf);
	return hdr_end + 4;
}

static void send_event(const char *name, const char *ptag, const char *value)
{
	char msg[200];
	int k = 0, ret;

	k = cat(msg, k, "<Event><Payload><Name>");
	k = cat(msg, k, name);
	k = cat(msg, k, "</Name><Params><");
	k = cat(msg, k, ptag);
	k = cat(msg, k, ">");
	k = cat(msg, k, value);
	k = cat(msg, k, "</");
	k = cat(msg, k, ptag);
	k = cat(msg, k, "></Params></Payload></Event>");
	k++;			/* send the terminating zero */
	if (hao < 0)
		hao = tcp("1100");
	ret = hao >= 0 ? write(hao, msg, k) : -1;
	if (ret != k && hao >= 0) {	/* reconnect once */
		close(hao);
		hao = tcp("1100");
		ret = hao >= 0 ? write(hao, msg, k) : -1;
	}
	logs(ret == k ? "send " : "SEND FAILED ", msg);
}

static int battery_logged;

/* battery level 0..8 from env_mgr, as SysService.lua reads it. (data_srv
 * does not answer GET /system/batterylevel; waiting for it blocked input.) */
static void update_battery(void)
{
	int result = 0, level = 0;

	env_get_battery_level(&result, &level);
	level &= 0xffff;	/* only 16 bits are written */
	if (battery_logged < 2) {
		lognum("battery env result ", result);
		lognum("battery env level ", level);
		battery_logged++;
	}
	if (result == 0 && level <= 8)
		battery = level;
}

static void update_clock(void)
{
	long t = time(NULL);
	struct tm *tm = localtime(&t);

	if (!tm)
		return;
	clock_text[0] = '0' + tm->tm_hour / 10;
	clock_text[1] = '0' + tm->tm_hour % 10;
	clock_text[2] = ':';
	clock_text[3] = '0' + tm->tm_min / 10;
	clock_text[4] = '0' + tm->tm_min % 10;
	clock_text[5] = 0;
}

/* ---- actions ---- */

static void set_status(const char *s, unsigned c)
{
	scopy(status_text, s, sizeof(status_text));
	status_color = c;
}

static void start_activity(int i)
{
	if (i == running) {	/* already on: just show it */
		if (stack[depth].kind != S_ACT)
			push(S_ACT, i);
		return;
	}
	logs("start activity ", act[i].label);
	send_event("StartActivity", "Id", act[i].id);
	if (act[i].id[0] == '-') {
		running = -1;
		set_status("Powering off", C_YELLOW);
		pop_to(0);
	} else {
		running = i;
		set_status("Starting", C_YELLOW);
		if (stack[depth].kind != S_ACT || stack[depth].index != i)
			push(S_ACT, i);
	}
}

/* hard key: device commands on a device screen, else the running activity */
static void hard_key(int scan)
{
	struct screen *s = &stack[depth];
	int i, j, k;

	for (i = 0; i < NKEYMAP; i++)
		if (keymap[i].scan == scan)
			break;
	if (i == NKEYMAP)
		return;
	if (s->kind == S_DEVICE && nav.x == nav.target) {
		struct device *d = &dev[s->index];
		const char *names[2] = { keymap[i].name, NULL };
		for (k = 0; k < (int)(sizeof(aliases) / sizeof(aliases[0])); k++)
			if (seq(aliases[k].from, keymap[i].name))
				names[1] = aliases[k].to;
		for (k = 0; k < 2 && names[k]; k++)
			for (j = 0; j < d->ncmd; j++) {
				/* ActionId is <device>_<Command>_Hold */
				const char *a = d->cmd[j].action + slen(d->id) + 1, *e = a;
				int n = slen(names[k]);
				while (*e && *e != '_')
					e++;
				if (e - a == n && find(a, e, names[k]) == a) {
					send_event("SendIR", "ActionId", d->cmd[j].action);
					set_status(d->cmd[j].label, C_ACCENT);
					return;
				}
			}
		set_status("Not on this device", C_TEXT2);
		return;
	}
	if (running < 0) {
		set_status("No activity", C_TEXT2);
		return;
	}
	for (j = 0; j < act[running].nhard; j++)
		if (seq(act[running].hard[j].name, keymap[i].name)) {
			send_event("SendIR", "ActionId", act[running].hard[j].action);
			set_status(keymap[i].name, C_ACCENT);
			return;
		}
	set_status("Not assigned", C_TEXT2);
}

static int repeats(int scan)
{
	int i;

	for (i = 0; i < NKEYMAP; i++)
		if (keymap[i].scan == scan)
			return keymap[i].repeat;
	return 0;
}

static void commit(int kind, int index)
{
	struct screen *s = &stack[depth];

	switch (kind) {
	case T_ACT: start_activity(index); break;
	case T_POWER: start_activity(power_off_index()); break;
	case T_BACK: pop_to(depth - 1); break;
	case T_DEVICES: push(S_DEVICES, 0); break;
	case T_DEV: push(S_DEVICE, index); break;
	case T_SOFT:
		send_event("SendIR", "ActionId", act[s->index].soft[index].action);
		set_status(act[s->index].soft[index].label, C_ACCENT);
		break;
	case T_CHAN:
		send_event("SendChannel", "Channel", act[s->index].chan[index].number);
		set_status(act[s->index].chan[index].station, C_ACCENT);
		break;
	case T_CMD:
		send_event("SendIR", "ActionId", dev[s->index].cmd[index].action);
		set_status(dev[s->index].cmd[index].label, C_ACCENT);
		break;
	}
}

static void hao_message(const char *m, int n)
{
	char name[32], send[16];
	const char *end = m + n;

	copy_tag(name, sizeof(name), m, end, "<Name>", "</Name>");
	if (seq(name, "ReceiverLinkDown")) {
		set_status("Blaster offline", C_RED);
	} else if (seq(name, "Queue")) {
		copy_tag(send, sizeof(send), m, end, "<Send>", "</Send>");
		if (seq(send, "Done"))
			set_status("Ready", C_GREEN);
		else if ((seq(send, "IR") || seq(send, "RF")) && status_color != C_RED)
			set_status("Sending", C_YELLOW);
	} else if (name[0]) {
		logs("hao ", name);
	}
}

static int hao_poll(void)
{
	int n, i, start;

	if (hao < 0 || !readable(hao, 0))
		return 0;
	n = read(hao, rx + rxlen, sizeof(rx) - 1 - rxlen);
	if (n <= 0) {
		logs("hao closed", NULL);
		close(hao);
		hao = -1;
		rxlen = 0;
		return 0;
	}
	rxlen += n;
	for (start = 0, i = 0; i < rxlen; i++)
		if (rx[i] == 0) {
			hao_message(rx + start, i - start);
			start = i + 1;
		}
	if (start > 0) {
		for (i = start; i < rxlen; i++)
			rx[i - start] = rx[i];
		rxlen -= start;
	}
	if (rxlen >= (int)sizeof(rx) - 1)
		rxlen = 0;
	return 1;
}

/* touch handling; returns 1 to quit */
static int touch(int x, int y, int down, int was_down)
{
	struct screen *s = &stack[depth];
	long t = now_ms();
	int kind, index;

	if (down && !was_down) {			/* touch-down */
		g_x0 = g_lx = x;
		g_y0 = g_ly = y;
		g_lt = t;
		g_vx = g_vy = 0;
		g_sy0 = s->sy;
		s->sv = 0;				/* catch a coasting list */
		if (depth > 0 && x < EDGE && nav.x == nav.target) {
			gesture = G_EDGE;
			press_kind = T_NONE;
		} else {
			gesture = G_TAP;
			hit(x, y, 0, &press_kind, &press_index);
		}
		return 0;
	}
	if (down) {					/* move */
		int dt = (int)(t - g_lt);
		if (dt > 0) {
			g_vx = (g_vx + (x - g_lx) * 1000 / dt) / 2;
			g_vy = (g_vy + (y - g_ly) * 1000 / dt) / 2;
		}
		g_lx = x;
		g_ly = y;
		g_lt = t;
		if (gesture == G_TAP) {
			int dy = y - g_y0;
			if ((dy > SLOP || dy < -SLOP) && scroll_max(s) > 0) {
				gesture = G_SCROLL;
				press_kind = T_NONE;
			} else {
				hit(x, y, SLOP, &kind, &index);
				if (kind != press_kind || index != press_index)
					press_kind = T_NONE;
			}
		}
		if (gesture == G_SCROLL) {
			int sy = (g_sy0 >> 8) - (y - g_y0), mx = scroll_max(s);
			int dim = content_bottom(s) - content_top(s);
			if (sy < 0)
				sy = rubber(sy, dim);
			else if (sy > mx)
				sy = mx + rubber(sy - mx, dim);
			s->sy = sy << 8;
		} else if (gesture == G_EDGE) {
			int dx = x - g_x0;
			if (dx < 0) dx = 0;
			if (dx > gfx_w) dx = gfx_w;
			nav.x = (depth * gfx_w - dx) << 8;
			nav.v = 0;
		}
		return 0;
	}
	/* release */
	if (gesture == G_TAP && press_kind != T_NONE) {
		int k = press_kind, i = press_index;
		press_kind = T_NONE;
		gesture = G_NONE;
		if (k == T_CLASSIC)
			return 1;
		commit(k, i);
		return 0;
	}
	if (gesture == G_SCROLL) {
		s->sv = -g_vy * 256;			/* hand the finger's velocity to the list */
	} else if (gesture == G_EDGE) {
		/* project where the swipe would come to rest (deceleration 0.998/ms) */
		int dx = x - g_x0 + g_vx / 2;
		if (dx > gfx_w / 2)
			pop_to(depth - 1);
		else
			nav.target = depth * gfx_w << 8;
		nav.v = -g_vx * 256;
	}
	press_kind = T_NONE;
	gesture = G_NONE;
	return 0;
}

int main(int argc, char **argv, char **envp)
{
	unsigned ev[16];
	gfi_group_t grp;
	gfi_layer_t il;
	const char *body;
	int i, n, held = -1, held_ms = 0, touching = 0, quit = 0, dirty = 1, frames = 0;
	long last_frame = 0, last_tick = 0, t;

	(void)argc; (void)argv; (void)envp;
	if ((n = gfx_init()) != 0 || (n = dr_init()) != 0) {
		char b[4] = { '0' + n / 10, '0' + n % 10, 0, 0 };
		logs("graphics init failed at step ", b);
		return n;
	}
	dr_fill(0, 0, gfx_w - 1, gfx_h - 1, C_BG, 256);
	dr_text(&font_title, M, 56, "Harmony", -1, C_TEXT, 256);
	dr_text(&font_body, M, 84, "Loading...", 0, C_TEXT2, 256);
	dr_present(0, 0, gfx_w - 1, gfx_h - 1);

	logs("ohui v4 start", NULL);
	body = http_get("/userconfig", cfg, sizeof(cfg), &n);
	if (body && n > 0)
		parse_config(body, n);
	for (i = 0; i < nact; i++)
		logs("activity ", act[i].label);
	for (i = 0; i < ndev; i++)
		logs("device ", dev[i].label);
	hao = tcp("1100");
	logs(hao >= 0 ? "hao connected" : "hao NOT connected", NULL);
	if (hao < 0)
		set_status("No connection", C_RED);
	env_service_connect("127.0.0.1");
	update_clock();
	update_battery();
	stack[0].kind = S_HOME;
	if (gfi_group_attach(&grp, 1, 0) != 0 ||
	    gfi_layer_attach(&il, grp, 0, gfx_main_layer) != 0 ||
	    gfi_layer_set_attrs(il, 0, 0, gfx_w - 1, gfx_h - 1, 7) != 0 ||
	    gfi_set_keyboard_focus(grp, gfx_main_layer) != 0) {
		logs("input setup failed", NULL);
		return 4;
	}
	last_tick = now_ms();

	while (!quit) {
		int key = -1, moving;

		/* 1. input first */
		for (i = 0; i < 16; i++)
			ev[i] = 0;
		while (gfi_layer_get_event(&il, 1, ev, GFI_NONBLOCK) == 0) {
			if (ev[0] == GFI_EV_KEY) {
				int sc = ev[5] & 0x7f;
				if ((ev[6] & 1) && !(ev[5] & 0x80)) {
					if (sc != held) {
						key = sc;
						held_ms = 0;
					}
					held = sc;
				} else if (sc == held) {
					held = -1;
				}
			} else if (ev[0] == GFI_EV_POINTER) {
				int down = ev[4] != 0;
				if (touch(ev[2], ev[3], down, touching))
					quit = 1;
				touching = down;
				dirty = 1;
			}
			for (i = 0; i < 16; i++)
				ev[i] = 0;
		}
		if (key >= 0) {
			char b[4];
			b[0] = "0123456789ABCDEF"[key >> 4];
			b[1] = "0123456789ABCDEF"[key & 15];
			b[2] = 0;
			logs("key ", b);
			dirty = 1;
			if (key == SCAN_OFF && power_off_index() >= 0)
				start_activity(power_off_index());
			else if (key == SCAN_ACTIVITIES)
				pop_to(0);
			else if (key == SCAN_UNDER_L)
				pop_to(depth - 1);
			else
				hard_key(key);
		} else if (held >= 0 && repeats(held)) {
			held_ms += POLL_MS;
			if (held_ms >= REPEAT_DELAY) {
				hard_key(held);
				held_ms = REPEAT_DELAY - REPEAT_MS;
				dirty = 1;
			}
		}

		/* 2. hao, clock and battery */
		if (hao_poll())
			dirty = 1;
		t = now_ms();
		if (t - last_tick > 30000) {
			last_tick = t;
			update_clock();
			update_battery();
			dirty = 1;
		}

		/* 3. motion */
		moving = 0;
		if (gesture != G_EDGE && nav.x != nav.target) {
			spring_step(&nav);
			moving = 1;
		}
		settle_stack();
		if (scroll_step(&stack[depth]))
			moving = 1;

		/* 4. draw: always while moving, else at most every FRAME_MS */
		if ((dirty || moving) && (moving || t - last_frame >= FRAME_MS)) {
			long t0 = now_ms();
			render();
			dr_present(0, 0, gfx_w - 1, gfx_h - 1);
			if (frames < 3)
				lognum("frame ms ", (int)(now_ms() - t0));
			frames++;
			last_frame = t;
			dirty = 0;
		}
		usleep(POLL_MS * 1000);
	}
	logs("ohui end (classic ui)", NULL);
	if (hao >= 0)
		close(hao);
	gf_dev_detach(gfx_dev);
	return 0;
}
#endif
