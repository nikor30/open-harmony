/* Harmony 900: native user interface (replaces the Flash UI).
 *
 * Version 9 is a stateless remote for a home cinema run by Home Assistant:
 * the home screen is the foyer of the HA "Kino" panel: in front of the
 * curtain one large ticket per activity, one per page, with "end the show"
 * below. A swipe left or right (or a side arrow key) turns the page, a tap
 * on the ticket starts it; the last page holds Settings and Classic UI.
 * A ticket, the end button and
 * every hard key send one infrared code and nothing else:
 *   NEC  4F 48 <code> <~code>     code 0x10 = off, 0x11.. = activities,
 *                                 0x80 | scancode = hard key
 * An IR gateway hears it and Home Assistant decides what the key means for
 * the activity that runs. The codes are not in the remote's Logitech
 * configuration; apply.py adds a "SendCode" event to hao (HAO.lua) that
 * sends a protocol code as it is (XML events to 127.0.0.1:1100, terminated
 * by a zero byte). The frame around the four bytes is the one of the NEC
 * protocol in this remote's configuration (protocol 4).
 *
 * The device and activity screens of version 8 (userconfig, SendIR,
 * SendChannel) are still in this file but not reachable.
 *
 * Screens form a stack (Activities > activity, Activities > Devices >
 * device, Activities > Settings) that slides with a critically damped
 * spring; a swipe from the left edge drags back 1:1 and projects the
 * release velocity. Lists scroll 1:1 with momentum and rubber-banding; the
 * scroll indicator flashes when a screen opens and edges fade where more
 * content is. In scrollable lists a button lights up only after 90 ms
 * without movement (like iOS), so scrolling never flashes or fires it.
 *
 * Hardware: the LEDs behind the four soft buttons show what they do now
 * (under-left white = back, under-right orange = open the running activity,
 * side arrows white = a page that way), set through data_srv like the
 * Flash UI does (/system/leftbottombutton etc., 0 off, 1 white, 2 orange).
 * Charging comes from env_get_charger_state, the battery level (0..8) from
 * env_get_battery_level. With sounds on (/system/sound), the buzzer ticks
 * when a tap commits, chimes when an activity is ready and buzzes low on
 * errors (audio_buzzer_generate_sound(0, Hz, ms)).
 *
 * Settings: brightness (live while dragging, saved on release), display
 * timeout, sounds; values are read from data_srv's cache files and written
 * back with POST /system/<name> (plain text, as sys_srv does).
 *
 * "Classic UI" ends ohui; flash.sh then starts the Flash player, as it does
 * if ohui fails. Log: /usr/local/cache/ohui.log. */
#include <ohdraw.h>

void *memcpy(void *, const void *, __SIZE_TYPE__);

#define VERSION		"10"
#define POLL_MS		10
#define FRAME_MS	33
#define REPEAT_DELAY	400
#define REPEAT_MS	150
#define SLOP		6
#define TAP_DELAY	90		/* ms before a press shows in a scrollable list */
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
#define C_SEP		0x38383a
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

/* the tickets of the Kino panel: name, stub text, gradient (cm: middle stop or 0), IR code */
static const struct kino { const char *name, *stub; unsigned c1, cm, c2; int code; } kino[] = {
	{"Film", "Blu-ray 4K", 0xe3241b, 0, 0xff8a1f, 0x11},
	{"Apple TV", "Streamen", 0x5e4be0, 0, 0xc058f3, 0x12},
	{"Spiel", "Retro-Gaming", 0x0a6cf0, 0, 0x39c4f5, 0x13},
	{"PS3 3D", "3D-Brille", 0xff2d55, 0xa23bd1, 0x19b5ab, 0x14},
};
#define NKINO		((int)(sizeof(kino) / sizeof(kino[0])))
#define KINO_OFF	0x10
#define C_GOLD		0xffcc66
#define C_CREAM		0xfff7ec
#define C_ROOM		0x09060a	/* the dark the curtain falls into */

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
static int running = -1;	/* ticket last tapped (the remote cannot see what really runs) */
static long sent_t;		/* when the last code went out (lamp) */
static int hao = -1;
static char rx[8192];
static int rxlen;
static char logbuf[24 * 1024];
static int loglen;
static char clock_text[8] = "";
static int battery = -1;	/* level 0..8 (cache/batt_lvls.dat: mV thresholds for 1..8) */
static int charging;
static int docked, dock_dismissed;	/* cradle */
static struct spring night;		/* night clock opacity 0..256*256 */
static int light = -1;			/* ambient light sensor level */
static int light_raw = -1;		/* env_get_light_reading */
static int night_on;			/* cradle mode applied (stay awake, dimmed) */
#define NIGHT_BRIGHTNESS 12
static long now;		/* ms, updated every loop */

static char status_text[24] = "Ready";
static unsigned status_color = C_TEXT2;

/* settings */
static int set_brightness = 60;		/* 0..100, /system/backlightlevel */
static int set_timeout = 30;		/* s, /system/backlighttimeout */
static int set_sound = 0;		/* /system/sound */
static const int timeouts[] = { 10, 30, 60, 120, 300 };
static const char *const timeout_labels[] = { "10s", "30s", "1m", "2m", "5m" };
#define NTIMEOUT 5
static struct spring seg_spring;	/* x of the selected segment, 1/256 px */
static struct spring sw_spring;		/* sound switch knob 0..256*256 */

/* ---- screens ---- */

enum { S_HOME, S_ACT, S_DEVICES, S_DEVICE, S_SETTINGS };
struct screen {
	int kind, index;
	int sy, sv;		/* scroll offset and velocity, 1/256 px and 1/256 px/s */
	int content_h;		/* set when drawn */
	long shown;		/* when the screen was pushed (scroll indicator flash) */
	int st, snapping;	/* page target (1/256 px) while snapping to it */
};
static struct screen stack[4];
static int depth;		/* index of the top screen */
static struct spring nav;	/* depth * gfx_w * 256 when settled */
static struct spring pager;	/* home: page * PG_STEP * 256 when settled */

/* touch */
enum { T_NONE, T_ACT, T_POWER, T_CLASSIC, T_BACK, T_SOFT, T_CHAN, T_DEVICES, T_DEV, T_CMD,
       T_SETTINGS, T_SLIDER, T_SEG, T_SOUND };
enum { G_NONE, G_TAP, G_SCROLL, G_EDGE, G_SLIDER, G_PAGE };
static int press_kind = T_NONE, press_index;
static long press_t;		/* when the press may show */
static int gesture = G_NONE;
static int g_x0, g_y0, g_sy0, g_grab, g_p0;
#define NHIST 6
static int h_x[NHIST], h_y[NHIST], h_n;
static long h_t[NHIST];

/* LEDs behind the soft buttons */
enum { LED_LEFT, LED_RIGHT, LED_UNDER_L, LED_UNDER_R };
static const char *const led_path[] = {
	"/system/leftarrow", "/system/rightarrow", "/system/leftbottombutton", "/system/rightbottombutton"
};
static int led_state[4] = { -1, -1, -1, -1 };

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
#define SCAN_SIDE_L	0x02
#define SCAN_SIDE_R	0x0D
#define SCAN_UNDER_L	0x1B
#define SCAN_UNDER_R	0x1C

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

static int atoi_s(const char *s)
{
	int v = 0;

	while (*s == ' ')
		s++;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + *s++ - '0';
	return v;
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

/* buttons of one control group (group tag), or of all groups (group NULL) */
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
		x->pos = atoi_s(pos);
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
#define ROW_H		44
#define TILE_W		((gfx_w - 2 * M - 8) / 2)
#define TILE_H		38
#define TILE_STEP	44

static int has_power_bar(const struct screen *s) { return s->kind == S_HOME; }
static int has_nav(const struct screen *s) { return s->kind != S_HOME; }
static int content_top(const struct screen *s) { return STATUS_H + (has_nav(s) ? NAV_H : 0); }
static int content_bottom(const struct screen *s) { return has_power_bar(s) ? POWER_Y - 6 : gfx_h - 1; }

/* home content */
#define TK_X1		22		/* the ticket of a page; its neighbours peek in at the sides */
#define TK_X2		(gfx_w - 23)
#define TK_Y1		8
#define TK_Y2		226
#define PG_STEP		(TK_X2 - TK_X1 + 13)
#define NPAGE		(NKINO + 1)	/* the tickets, then More */
#define PG_DOTS_Y	(POWER_Y - 13)
#define H_MORE		70		/* section label baseline */
#define H_ROW(i)	(H_MORE + 8 + (i) * ROW_H)
#define A_TITLE		30
#define A_SECTION1	46
#define A_TILE_Y(i)	(56 + ((i) >> 1) * TILE_STEP)
#define TILE_X(i)	(M + ((i) & 1) * (TILE_W + 8))
/* settings content */
#define ST_BRIGHT	54		/* card top */
#define ST_SLIDER_Y	(ST_BRIGHT + 48)
#define ST_SEG		146
#define ST_SOUND	204
#define ST_ABOUT	266
#define SL_X1		(M + 16)
#define SL_X2		(gfx_w - M - 17)

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

static int seg_x(int i)
{
	int w = (gfx_w - 2 * M - 4) / NTIMEOUT;
	return M + 2 + i * w;
}

static int home_page(void) { return pager.target / (PG_STEP << 8); }

/* control under screen point (x, y) on the top screen */
static void hit(int x, int y, int slop, int *kind, int *index)
{
	struct screen *s = &stack[depth];
	int i, cy;

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
		i = home_page();
		if (pager.x - pager.target > 8 << 8 || pager.target - pager.x > 8 << 8)
			return;		/* still turning the page */
		if (i < NKINO) {
			if (in(x, cy, TK_X1, TK_Y1, TK_X2, TK_Y2, slop > 3 ? 3 : slop)) {
				*kind = T_ACT;
				*index = i;
			}
			return;
		}
		for (i = 0; i < 2; i++)
			if (in(x, cy, M, H_ROW(i), gfx_w - M, H_ROW(i) + ROW_H - 1, slop > 4 ? 4 : slop)) {
				*kind = i == 0 ? T_SETTINGS : T_CLASSIC;
				return;
			}
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
	case S_SETTINGS:
		if (in(x, cy, M, ST_SLIDER_Y - 14, gfx_w - M, ST_SLIDER_Y + 14, slop)) {
			*kind = T_SLIDER;
			return;
		}
		for (i = 0; i < NTIMEOUT; i++)
			if (in(x, cy, seg_x(i), ST_SEG, seg_x(i + 1) - 1, ST_SEG + 31, 0)) {
				*kind = T_SEG;
				*index = i;
				return;
			}
		if (in(x, cy, M, ST_SOUND, gfx_w - M, ST_SOUND + ROW_H - 1, slop))
			*kind = T_SOUND;
		return;
	}
}

/* ---- drawing ---- */

static int pressed(int kind, int index)
{
	return press_kind == kind && press_index == index && gesture == G_TAP && now >= press_t;
}

/* ---- icons, drawn from shapes (centre cx, cy) ---- */

static void icon_power(int cx, int cy, unsigned c, int a)
{
	dr_ring(cx, cy, 8, 6, 3, c, a);
	dr_line(cx, cy - 9, cx, cy - 2, 2, c, a);
}

static void icon_gear(int cx, int cy, unsigned c, int a)
{
	static const signed char tx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
	static const signed char ty[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
	int i;

	for (i = 0; i < 8; i++)
		dr_line(cx, cy, cx + tx[i], cy + ty[i], 3, c, a);
	dr_circle(cx, cy, 7, c, a);
	dr_circle(cx, cy, 3, C_BG, 256);
}

static void icon_tv(int cx, int cy, unsigned c, int a)
{
	dr_rrect(cx - 10, cy - 7, cx + 9, cy + 5, 3, c, a);
	dr_rrect(cx - 8, cy - 5, cx + 7, cy + 3, 1, C_BG, 256);
	dr_line(cx - 4, cy + 8, cx + 3, cy + 8, 2, c, a);
}

static void icon_classic(int cx, int cy, unsigned c, int a)
{
	dr_ring(cx, cy, 8, 6, 0, c, a);
	dr_line(cx, cy - 4, cx, cy, 2, c, a);
	dr_line(cx, cy, cx + 3, cy + 2, 2, c, a);
}

static void icon_bolt(int x, int y, unsigned c, int a)
{
	dr_line(x + 3, y, x, y + 5, 2, c, a);
	dr_line(x, y + 5, x + 4, y + 5, 2, c, a);
	dr_line(x + 4, y + 5, x + 1, y + 10, 2, c, a);
}

static void chevron_right(int cx, int cy, unsigned c, int a)
{
	dr_line(cx - 2, cy - 5, cx + 2, cy, 2, c, a);
	dr_line(cx + 2, cy, cx - 2, cy + 5, 2, c, a);
}

static void draw_status_bar(void)
{
	int x = gfx_w - M - 24;

	dr_fill(0, 0, gfx_w - 1, STATUS_H - 1, C_BG, 256);
	dr_text(&font_small_medium, M, 14, clock_text, 0, C_TEXT, 256);
	if (battery >= 0) {
		int fillw = 18 * battery / 8;
		unsigned c = charging ? C_GREEN : battery <= 1 ? C_RED : battery <= 2 ? C_YELLOW : C_TEXT;
		dr_rrect(x, 5, x + 21, 15, 3, C_TEXT, 100);		/* battery body */
		dr_rrect(x + 1, 6, x + 20, 14, 2, C_BG, 256);
		if (fillw > 0)
			dr_rrect(x + 2, 7, x + 1 + fillw, 13, 2, c, 256);
		dr_fill(x + 22, 8, x + 23, 12, C_TEXT, 100);		/* terminal */
		if (charging)
			icon_bolt(x - 9, 5, C_GREEN, 256);
	}
}

/* "end the show": smoked glass with a bright top edge, faint while nothing runs */
static void draw_power_bar(void)
{
	const char *t = "Vorstellung beenden";
	int w = dr_text_width(&font_button, t, 0) + 24, y, x, a = running >= 0 ? 256 : 120;

	for (y = POWER_Y - 16; y < POWER_Y - 6; y++)	/* content fades out above the bar */
		dr_hspan(0, gfx_w - 1, y, C_ROOM, (y - (POWER_Y - 16)) * 26);
	dr_fill(0, POWER_Y - 6, gfx_w - 1, gfx_h - 1, C_ROOM, 256);
	dr_rrect(M, POWER_Y, gfx_w - 1 - M, POWER_Y + POWER_H - 1, 12, 0xffffff, 44);
	dr_rrect(M + 1, POWER_Y + 1, gfx_w - 2 - M, POWER_Y + POWER_H - 2, 11,
		 pressed(T_POWER, 0) ? 0x43323a : 0x241a1e, 256);
	dr_hspan(M + 12, gfx_w - 1 - M - 12, POWER_Y, 0xffffff, 40);
	x = (gfx_w - w) / 2;
	icon_power(x + 8, POWER_Y + 20, 0xff6961, a);
	dr_text(&font_button, x + 24, POWER_Y + 26, t, 0, C_CREAM, a);
	/* page dots: the page shown is bright, the ticket that runs is gold */
	for (y = 0; y < NPAGE; y++) {
		int here = y == ((pager.x >> 8) + PG_STEP / 2) / PG_STEP;
		dr_circle(gfx_w / 2 - (NPAGE - 1) * 6 + y * 12, PG_DOTS_Y, here ? 3 : 2,
			  y == running ? C_GOLD : 0xffffff, here || y == running ? 256 : 90);
	}
	if (now - sent_t < 400)	/* a code just went out */
		dr_circle(gfx_w - M - 8, PG_DOTS_Y, 3, C_GOLD, 256);
}

static const char *back_label(const struct screen *s)
{
	return s->kind == S_DEVICE ? "Devices" : "Kino";
}

static void draw_nav(const struct screen *s, const char *title)
{
	int a = pressed(T_BACK, 0) ? 110 : 256, w;

	dr_fill(0, STATUS_H, gfx_w - 1, STATUS_H + NAV_H - 1, C_BG, 256);
	dr_line(M + 6, STATUS_H + 8, M + 1, STATUS_H + 14, 2, C_ACCENT, a);	/* "<" */
	dr_line(M + 1, STATUS_H + 14, M + 6, STATUS_H + 20, 2, C_ACCENT, a);
	dr_text(&font_body, M + 12, STATUS_H + 19, back_label(s), 0, C_ACCENT, a);
	if ((s->sy >> 8) > 0) {	/* scrolled: soft edge under the bar, title when the large one is gone */
		int t = (s->sy >> 8) - A_TITLE, al = t <= 0 ? 0 : t > 16 ? 256 : t * 16;
		if (title && al) {
			w = dr_text_width(&font_small_medium, title, 0);
			if (w < 100)
				dr_text(&font_small_medium, (gfx_w - w) / 2 + 20, STATUS_H + 18, title, 0, C_TEXT, al);
		}
		for (w = 0; w < 8; w++)
			dr_hspan(0, gfx_w - 1, STATUS_H + NAV_H + w, C_BG, 200 - w * 25);
	}
	if (s->kind == S_ACT || s->kind == S_DEVICE) {	/* status pill */
		w = dr_text_width(&font_caption, status_text, 0) + 22;
		if (w < 104) {
			dr_rrect(gfx_w - M - w, STATUS_H + 5, gfx_w - M - 1, STATUS_H + 23, 9, status_color, 46);
			dr_circle(gfx_w - M - w + 10, STATUS_H + 14, 3, status_color, 256);
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

static void card(int y, unsigned tint, const char *label, const char *sub, unsigned sub_c,
		 int dot, int hi)
{
	int inset = hi ? 2 : 0;
	char ini[4];

	dr_rrect(M + inset, y + inset, gfx_w - 1 - M - inset, y + CARD_H - 1 - inset, 14,
		 hi ? C_CARD_HI : C_CARD, 256);
	dr_hspan(M + 14, gfx_w - 1 - M - 14, y + inset, 0xffffff, 18);
	/* icon: tinted disc with a soft glow of its own colour */
	dr_circle(M + 27, y + 27, 20, tint, 40);
	dr_circle(M + 27, y + 27, 17, tint, 256);
	dr_circle(M + 27, y + 22, 12, 0xffffff, 26);		/* light from above */
	initial_of(ini, label);
	dr_text(&font_headline, M + 27 - dr_text_width(&font_headline, ini, 0) / 2, y + 33,
		ini, 0, C_TEXT, 256);
	dr_text_fit(&font_button, M + 54, y + 24, label, 0, C_TEXT, 256, gfx_w - 2 * M - 80);
	if (dot)
		dr_circle(M + 57, y + 37, 3, sub_c, 256);
	dr_text(&font_caption, M + 54 + (dot ? 9 : 0), y + 41, sub, 0, sub_c, 256);
	chevron_right(gfx_w - M - 14, y + 27, C_TEXT3, 256);
}

/* row of a grouped list; first/last round the group's corners */
static void row(int y, int first, int last, int hi, int icon, unsigned icon_bg, const char *label,
		const char *value)
{
	int r = 12;

	if (first && last)
		dr_rrect(M, y, gfx_w - 1 - M, y + ROW_H - 1, r, hi ? C_CARD_HI : C_CARD, 256);
	else if (first) {
		dr_rrect(M, y, gfx_w - 1 - M, y + ROW_H - 1 + r, r, hi ? C_CARD_HI : C_CARD, 256);
	} else if (last) {
		dr_rrect(M, y - r, gfx_w - 1 - M, y + ROW_H - 1, r, hi ? C_CARD_HI : C_CARD, 256);
	} else
		dr_fill(M, y, gfx_w - 1 - M, y + ROW_H - 1, hi ? C_CARD_HI : C_CARD, 256);
	if (!first)
		dr_hspan(M + 48, gfx_w - 1 - M, y, C_SEP, 256);
	if (icon >= 0) {
		dr_rrect(M + 10, y + 9, M + 35, y + 34, 7, icon_bg, 256);
		switch (icon) {
		case 0: icon_tv(M + 23, y + 21, C_TEXT, 256); break;
		case 1: icon_gear(M + 23, y + 22, C_TEXT, 256); break;
		case 2: icon_classic(M + 23, y + 22, C_TEXT, 256); break;
		}
	}
	dr_text(&font_body, M + (icon >= 0 ? 48 : 14), y + 27, label, 0, C_TEXT, 256);
	if (value)
		dr_text(&font_body, gfx_w - M - 24 - dr_text_width(&font_body, value, 0), y + 27, value, 0,
			C_TEXT2, 256);
	chevron_right(gfx_w - M - 14, y + 22, C_TEXT3, 256);
}

/* ---- Kino home: the foyer of the Home Assistant panel ---- */

/* red curtain folds at the top that fall into the dark of the room; built
 * once (dithered to RGB565), it stays in place while the content scrolls */
static unsigned short curtain[320 * 240];

static void curtain_init(void)
{
	static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
	int x, y, h1 = gfx_h * 30 / 100, h2 = gfx_h * 80 / 100;

	for (y = 0; y < gfx_h && y < 320; y++) {
		int t = y * 256 / (gfx_h * 6 / 10);
		unsigned base = dr_mix(0xa3121f, 0x5c0911, t > 256 ? 256 : t);
		int dark = y < h1 ? 80 * y / h1 : y < h2 ? 80 + 176 * (y - h1) / (h2 - h1) : 256;
		for (x = 0; x < gfx_w && x < 240; x++) {
			int p = x % 58, w = p < 26 ? p * 256 / 26 : (58 - p) * 256 / 32;	/* fold: 0 valley, 256 ridge */
			unsigned c = dr_mix(base, 0x000000, 107 * (256 - w) >> 8);
			int d = bayer[(y & 3) * 4 + (x & 3)], r, g, b;
			c = dr_mix(dr_mix(c, 0xffffff, 18 * w >> 8), C_ROOM, dark);
			r = ((c >> 16) & 255) + (d >> 1);
			g = ((c >> 8) & 255) + (d >> 2);
			b = (c & 255) + (d >> 1);
			if (r > 255) r = 255;
			if (g > 255) g = 255;
			if (b > 255) b = 255;
			curtain[y * 240 + x] = RGB((unsigned)(r << 16 | g << 8 | b));
		}
	}
}

static void draw_curtain(void)
{
	int x1 = dr_cx1 > dr_ox ? dr_cx1 : dr_ox, x2 = dr_cx2 < dr_ox + 239 ? dr_cx2 : dr_ox + 239, y;

	for (y = dr_cy1; y <= dr_cy2 && y < 320 && x1 <= x2; y++)
		memcpy(&dr_px[y * dr_stride + x1], &curtain[y * 240 + x1 - dr_ox], (x2 - x1 + 1) * 2);
}

/* rounded rectangle with a gradient c1 (cm) c2 from top-left to bottom-right. Every row is the same run of
 * colours one pixel further along, so the run is worked out once and the rows are copied from it. */
static void grad_rrect(int x1, int y1, int x2, int y2, int r, unsigned c1, unsigned cm, unsigned c2)
{
	static unsigned run[240 + 320];
	static unsigned short run16[240 + 320];
	const unsigned char *cov = dr_corner(r);
	int span = x2 - x1 + y2 - y1, t, i, y;

	if (span <= 0 || span >= 240 + 320)
		return;
	for (t = 0; t <= span; t++) {
		int k = t * 256 / span;
		run[t] = !cm ? dr_mix(c1, c2, k) : k < 133 ? dr_mix(c1, cm, k * 256 / 133) : dr_mix(cm, c2, (k - 133) * 256 / 123);
		run16[t] = RGB(run[t]);
	}
	for (y = y1; y <= y2; y++) {
		int j = y - y1 < r ? y - y1 : y2 - y < r ? y2 - y : -1, in = j < 0 ? 0 : r;
		int a = x1 + in + dr_ox, b = x2 - in + dr_ox, sy = y + dr_oy;
		const unsigned short *src = run16 + in + y - y1;

		for (i = 0; i < in; i++) {	/* corners, anti-aliased */
			dr_pixel(x1 + i, y, run[i + y - y1], cov[j * r + i] << 4);
			dr_pixel(x2 - i, y, run[x2 - i - x1 + y - y1], cov[j * r + i] << 4);
		}
		if (sy < dr_cy1 || sy > dr_cy2)
			continue;
		if (a < dr_cx1) {
			src += dr_cx1 - a;
			a = dr_cx1;
		}
		if (b > dr_cx2)
			b = dr_cx2;
		if (a <= b)
			memcpy(&dr_px[sy * dr_stride + a], src, (b - a + 1) * 2);
	}
}

/* ticket icons, k times the size of the drawing */
static void icon_clapper(int cx, int cy, int k)
{
	int i;

	dr_rrect(cx - 12 * k, cy - 2 * k, cx + 12 * k, cy + 11 * k, 3 * k, 0xffffff, 256);
	dr_rrect(cx - 12 * k, cy - 11 * k, cx + 12 * k, cy - 5 * k, 2 * k, 0xffffff, 256);
	for (i = -8; i <= 8; i += 8)
		dr_line(cx + (i - 2) * k, cy - 5 * k, cx + (i + 2) * k, cy - 11 * k, 3 * k, 0x000000, 120);
}

static void icon_apple(int cx, int cy, int k)
{
	dr_circle(cx - 4 * k, cy, 7 * k, 0xffffff, 256);
	dr_circle(cx + 4 * k, cy, 7 * k, 0xffffff, 256);
	dr_circle(cx - 3 * k, cy + 6 * k, 6 * k, 0xffffff, 256);
	dr_circle(cx + 3 * k, cy + 6 * k, 6 * k, 0xffffff, 256);
	dr_line(cx + k, cy - 8 * k, cx + 5 * k, cy - 12 * k, 3 * k, 0xffffff, 256);
}

static void icon_invader(int cx, int cy, int k)
{
	static const unsigned short rows[8] = { 0x104, 0x088, 0x1fc, 0x376, 0x7ff, 0x5fd, 0x505, 0x0d8 };
	int x, y;

	k *= 3;
	for (y = 0; y < 8; y++)
		for (x = 0; x < 11; x++)
			if (rows[y] & (0x400 >> x))
				dr_fill(cx + (2 * x - 11) * k / 2, cy + (y - 4) * k,
					cx + (2 * x - 11) * k / 2 + k - 1, cy + (y - 3) * k - 1, 0xffffff, 256);
}

/* 3D glasses, one lens red and one cyan */
static void icon_glasses(int cx, int cy, int k)
{
	dr_rrect(cx - 13 * k, cy - 6 * k, cx + 13 * k, cy + 6 * k, 3 * k, 0xffffff, 256);
	dr_rrect(cx - 11 * k, cy - 4 * k, cx - 2 * k, cy + 4 * k, 2 * k, 0xff3b30, 256);
	dr_rrect(cx + 2 * k, cy - 4 * k, cx + 11 * k, cy + 4 * k, 2 * k, 0x19c3d6, 256);
}

/* colour of the curtain at a point of the home screen's content */
static unsigned curtain_at(int x, int y)
{
	unsigned short p;

	x = x < 0 ? 0 : x > 239 ? 239 : x;
	y += dr_oy;
	p = curtain[(y < 0 ? 0 : y > 319 ? 319 : y) * 240 + x];
	return (p & 0xf800) << 8 | (p & 0x07e0) << 5 | (p & 0x001f) << 3;
}

/* admission ticket, one to a page: colour body, perforation with two punched notches, stub underneath */
static void ticket(int i, int hi)
{
	const struct kino *k = &kino[i];
	int in = hi ? 3 : 0, x1 = TK_X1 + in, y1 = TK_Y1 + in, x2 = TK_X2 - in, y2 = TK_Y2 - in;
	int cx = (x1 + x2 + 1) / 2, py = y2 - 44, x;
	const char *stub = running == i ? "L\xc3\xa4uft gerade" : k->stub;

	if (running == i) {	/* white frame */
		dr_rrect(x1, y1, x2, y2, 20, 0xffffff, 256);
		grad_rrect(x1 + 4, y1 + 4, x2 - 4, y2 - 4, 16, k->c1, k->cm, k->c2);
	} else {
		grad_rrect(x1, y1, x2, y2, 20, k->c1, k->cm, k->c2);
	}
	switch (i) {
	case 0: icon_clapper(cx, y1 + 68, 3); break;
	case 1: icon_apple(cx, y1 + 64, 3); break;
	case 2: icon_invader(cx, y1 + 66, 2); break;
	default: icon_glasses(cx, y1 + 66, 3);
	}
	dr_text(&font_title, cx - dr_text_width(&font_title, k->name, -1) / 2, py - 18, k->name, -1, 0xffffff, 256);
	for (x = x1 + 16; x + 5 <= x2 - 16; x += 10)
		dr_fill(x, py, x + 5, py + 1, 0xffffff, 128);
	{	/* notches show the curtain behind; only the half inside the ticket is drawn */
		int c1 = dr_cx1, c2 = dr_cx2;
		dr_clip(x1 + dr_ox > c1 ? x1 + dr_ox : c1, dr_cy1, x2 + dr_ox < c2 ? x2 + dr_ox : c2, dr_cy2);
		dr_circle(x1, py + 1, 9, curtain_at(x1 + dr_ox, py), 256);
		dr_circle(x2 + 1, py + 1, 9, curtain_at(x2 + dr_ox, py), 256);
		dr_clip(c1, dr_cy1, c2, dr_cy2);
	}
	dr_text(&font_button, cx - dr_text_width(&font_button, stub, 0) / 2, y2 - 15, stub, 0, 0xffffff, 235);
}

static void draw_home(struct screen *s)
{
	int i, ox = dr_ox;

	for (i = 0; i < NPAGE; i++) {
		int off = i * PG_STEP - (pager.x >> 8);
		if (TK_X2 + off < 0 || TK_X1 + off >= gfx_w)
			continue;
		dr_ox = ox + off;
		if (i < NKINO) {
			ticket(i, pressed(T_ACT, i));
			continue;
		}
		dr_text(&font_caption, M + 4, H_MORE, "MORE", 1, 0x9a8f8a, 256);
		row(H_ROW(0), 1, 0, pressed(T_SETTINGS, 0), 1, 0x636366, "Settings", NULL);
		row(H_ROW(1), 0, 1, pressed(T_CLASSIC, 0), 2, 0x48484a, "Classic UI", NULL);
	}
	dr_ox = ox;
	s->content_h = 0;	/* pages, no scrolling */
}

static void draw_activity(struct screen *s)
{
	struct activity *a = &act[s->index];
	int i, y;

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
}

static void draw_settings(struct screen *s)
{
	char v[32];
	int i, kx, sx;

	dr_text(&font_title, M, A_TITLE, "Settings", -1, C_TEXT, 256);
	/* brightness card: label, value, slider */
	section(ST_BRIGHT - 6, "Display");
	dr_rrect(M, ST_BRIGHT, gfx_w - 1 - M, ST_BRIGHT + 69, 12, C_CARD, 256);
	dr_text(&font_body, M + 14, ST_BRIGHT + 24, "Brightness", 0, C_TEXT, 256);
	cat(v, catnum(v, 0, set_brightness), "%");
	dr_text(&font_body, gfx_w - M - 14 - dr_text_width(&font_body, v, 0), ST_BRIGHT + 24, v, 0, C_TEXT2, 256);
	kx = SL_X1 + (SL_X2 - SL_X1) * set_brightness / 100;
	dr_rrect(SL_X1, ST_SLIDER_Y - 2, SL_X2, ST_SLIDER_Y + 2, 2, 0x48484a, 256);
	dr_rrect(SL_X1, ST_SLIDER_Y - 2, kx, ST_SLIDER_Y + 2, 2, C_ACCENT, 256);
	dr_circle(kx, ST_SLIDER_Y + 1, 12, 0x000000, 70);			/* knob shadow */
	dr_circle(kx, ST_SLIDER_Y, gesture == G_SLIDER ? 12 : 11, 0xffffff, 256);
	/* display timeout: segmented control with a sliding selection */
	section(ST_SEG - 6, "Turn display off after");
	dr_rrect(M, ST_SEG, gfx_w - 1 - M, ST_SEG + 31, 9, C_CARD, 256);
	sx = seg_spring.x >> 8;
	dr_rrect(sx, ST_SEG + 2, sx + (seg_x(1) - seg_x(0)) - 1, ST_SEG + 29, 7, 0x636366, 256);
	for (i = 0; i < NTIMEOUT; i++) {
		int w = dr_text_width(&font_small_medium, timeout_labels[i], 0);
		dr_text(&font_small_medium, seg_x(i) + (seg_x(1) - seg_x(0) - w) / 2, ST_SEG + 20,
			timeout_labels[i], 0, C_TEXT, timeouts[i] == set_timeout ? 256 : 190);
	}
	/* sounds switch */
	section(ST_SOUND - 6, "Feedback");
	dr_rrect(M, ST_SOUND, gfx_w - 1 - M, ST_SOUND + ROW_H - 1, 12,
		 pressed(T_SOUND, 0) ? C_CARD_HI : C_CARD, 256);
	dr_text(&font_body, M + 14, ST_SOUND + 27, "Sounds", 0, C_TEXT, 256);
	{
		int t = sw_spring.x >> 8, x2 = gfx_w - M - 12, x1 = x2 - 44;	/* t 0..256 */
		dr_rrect(x1, ST_SOUND + 9, x2, ST_SOUND + 35, 13, dr_mix(0x39393d, C_GREEN, t < 0 ? 0 : t > 256 ? 256 : t), 256);
		kx = x1 + 13 + (18 * t >> 8);
		dr_circle(kx, ST_SOUND + 23, 12, 0x000000, 60);
		dr_circle(kx, ST_SOUND + 22, 11, 0xffffff, 256);
	}
	/* about */
	section(ST_ABOUT - 6, "Remote");
	dr_rrect(M, ST_ABOUT, gfx_w - 1 - M, ST_ABOUT + 3 * ROW_H - 1, 12, C_CARD, 256);
	dr_text(&font_body, M + 14, ST_ABOUT + 27, "Battery", 0, C_TEXT, 256);
	if (battery >= 0) {
		int k = cat(v, catnum(v, 0, battery), " of 8");
		if (charging)
			cat(v, k, ", charging");
	} else
		cat(v, 0, "unknown");
	dr_text(&font_body, gfx_w - M - 14 - dr_text_width(&font_body, v, 0), ST_ABOUT + 27, v, 0, C_TEXT2, 256);
	dr_hspan(M + 14, gfx_w - 1 - M, ST_ABOUT + ROW_H, C_SEP, 256);
	dr_text(&font_body, M + 14, ST_ABOUT + ROW_H + 27, "Interface", 0, C_TEXT, 256);
	cat(v, cat(v, 0, "Native "), VERSION);
	dr_text(&font_body, gfx_w - M - 14 - dr_text_width(&font_body, v, 0), ST_ABOUT + ROW_H + 27, v, 0,
		C_TEXT2, 256);
	dr_hspan(M + 14, gfx_w - 1 - M, ST_ABOUT + 2 * ROW_H, C_SEP, 256);
	dr_text(&font_body, M + 14, ST_ABOUT + 2 * ROW_H + 27, "Light sensor", 0, C_TEXT, 256);
	if (light >= 0) {
		int k = catnum(v, 0, light);
		k = cat(v, k, " / ");
		catnum(v, k, light_raw);
	} else
		cat(v, 0, "-");
	dr_text(&font_body, gfx_w - M - 14 - dr_text_width(&font_body, v, 0), ST_ABOUT + 2 * ROW_H + 27, v, 0,
		C_TEXT2, 256);
	s->content_h = ST_ABOUT + 3 * ROW_H + 12;
}

/* colour wash in the screen's own colour behind the large title */
static void wash(const struct screen *s)
{
	unsigned c;
	int y, top = content_top(s), h = 70;

	if (s->kind == S_ACT)
		c = tints[s->index % 6];
	else if (s->kind == S_DEVICE)
		c = device_tint(&dev[s->index]);
	else
		return;
	for (y = 0; y < h; y++)
		dr_hspan(0, gfx_w - 1, top + y - (s->sy >> 8) / 2, c, (h - y) * 40 / h);
}

/* draw screen s with its content scrolled; dr_ox is the horizontal offset */
static void draw_screen(struct screen *s)
{
	int top = content_top(s), bottom = content_bottom(s), y, i;
	int cx1 = dr_cx1, cx2 = dr_cx2;		/* horizontal clip set by the caller */

	if (s->kind != S_HOME)		/* home: curtain, status bar and power bar cover the screen */
		dr_fill(0, 0, gfx_w - 1, gfx_h - 1, C_BG, 256);
	dr_clip(cx1, top, cx2, bottom);
	if (s->kind == S_HOME)
		draw_curtain();
	wash(s);
	dr_oy = top - (s->sy >> 8);
	switch (s->kind) {
	case S_HOME: draw_home(s); break;
	case S_ACT:
		dr_text_fit(&font_title, M, A_TITLE, act[s->index].label, -1, C_TEXT, 256, gfx_w - 2 * M);
		draw_activity(s);
		break;
	case S_DEVICES:
		dr_text(&font_title, M, A_TITLE, "Devices", -1, C_TEXT, 256);
		for (i = 0; i < ndev; i++) {
			char sub[32];
			cat(sub, catnum(sub, 0, dev[i].ncmd), " commands");
			card(A_SECTION1 + 4 + i * CARD_STEP, device_tint(&dev[i]), dev[i].label, sub,
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
	case S_SETTINGS: draw_settings(s); break;
	}
	dr_oy = 0;
	/* bottom edge: fade where more content follows */
	if ((s->sy >> 8) < scroll_max(s) && !has_power_bar(s))
		for (y = 0; y < 14; y++)
			dr_hspan(0, gfx_w - 1, bottom - 13 + y, C_BG, y * 18);
	dr_clip(cx1, 0, cx2, gfx_h - 1);
	/* scroll indicator: while moving, and briefly when the screen opens */
	{
		int mx = scroll_max(s), vis = bottom - top, a = 0;
		long age = now - s->shown;
		if (s->sv || gesture == G_SCROLL)
			a = 110;
		else if (age < 1200)
			a = age < 900 ? 110 : (int)(1200 - age) * 110 / 300;
		if (mx > 0 && a > 0) {
			int h = vis * vis / s->content_h, pos = s->sy >> 8;
			if (h < 20) h = 20;
			if (pos < 0) pos = 0;
			if (pos > mx) pos = mx;
			y = top + 2 + (vis - h - 4) * pos / mx;
			dr_rrect(gfx_w - 5, y, gfx_w - 3, y + h, 1, 0xffffff, a);
		}
	}
	if (has_nav(s))
		draw_nav(s, s->kind == S_ACT ? act[s->index].label : s->kind == S_DEVICE ? dev[s->index].label :
			    s->kind == S_DEVICES ? "Devices" : "Settings");
	if (has_power_bar(s))
		draw_power_bar();
}

/* sin(2*pi*k/32) * 1000 */
static const short sin32[32] = {
	0, 195, 383, 556, 707, 831, 924, 981, 1000, 981, 924, 831, 707, 556, 383, 195,
	0, -195, -383, -556, -707, -831, -924, -981, -1000, -981, -924, -831, -707, -556, -383, -195,
};

/* bedside screen in the cradle: large clock, date, charging ring of dots */
static void draw_night(int a)
{
	static const char *const wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const mo[] = { "January", "February", "March", "April", "May", "June", "July",
					  "August", "September", "October", "November", "December" };
	char d[40];
	int w, k, lit, cx = gfx_w / 2, cy = 236, r = 34;
	struct tm *tm = NULL;
#ifndef OHUI_HOST
	long t = time(NULL);
	tm = localtime(&t);
#endif
	dr_fill(0, 0, gfx_w - 1, gfx_h - 1, C_BG, a);
	if (a < 40)
		return;
	w = dr_text_width(&font_clock, clock_text, -2);
	dr_text(&font_clock, (gfx_w - w) / 2, 128, clock_text, -2, C_TEXT, a);
	if (tm) {
		k = cat(d, 0, wd[tm->tm_wday % 7]);
		k = cat(d, k, ", ");
		k = catnum(d, k, tm->tm_mday);
		k = cat(d, k, ". ");
		cat(d, k, mo[tm->tm_mon % 12]);
		w = dr_text_width(&font_body, d, 0);
		dr_text(&font_body, (gfx_w - w) / 2, 158, d, 0, C_TEXT2, a);
	}
	/* 32 dots; the share of the battery level lights up, the next one breathes while charging */
	lit = battery < 0 ? 0 : battery * 4;
	for (k = 0; k < 32; k++) {
		int x = cx + sin32[k] * r / 1000, y = cy - sin32[(k + 8) & 31] * r / 1000;
		unsigned c = k < lit ? (charging ? C_GREEN : C_TEXT) : 0x3a3a3c;
		int da = a;
		if (k == lit && charging && lit < 32)
			da = a * (128 + (int)((now / 4) % 256 < 128 ? (now / 4) % 128 : 127 - (now / 4) % 128)) >> 8;
		dr_circle(x, y, k < lit ? 3 : 2, k == lit && charging ? C_GREEN : c, da);
	}
	if (charging)
		icon_bolt(cx - 2, cy - 5, C_GREEN, a);
	else if (battery >= 0) {
		cat(d, catnum(d, 0, battery), "/8");
		w = dr_text_width(&font_caption, d, 0);
		dr_text(&font_caption, cx - w / 2, cy + 4, d, 0, C_TEXT2, a);
	}
	w = dr_text_width(&font_caption, "Tap to wake", 0);
	dr_text(&font_caption, (gfx_w - w) / 2, gfx_h - 12, "Tap to wake", 0, C_TEXT3, a);
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
	if (night.x > 0)
		draw_night(night.x >> 8 > 256 ? 256 : night.x >> 8);
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
	stack[depth].shown = now;
	nav.target = depth * gfx_w << 8;
}

static void pop_to(int d)
{
	if (d < 0)
		d = 0;
	nav.target = d * gfx_w << 8;	/* depth follows when the spring settles */
	stack[d].shown = now;
}

static void settle_stack(void)
{
	int d = nav.target / (gfx_w << 8);

	if (nav.x == nav.target && d < depth)
		depth = d;
}

/* scroll physics for a screen over n 10 ms steps; returns 1 while moving */
static int scroll_step(struct screen *s, int n)
{
	int mx = scroll_max(s) << 8;

	if (gesture == G_SCROLL)
		return 0;
	if (s->snapping) {			/* paging: spring to the page */
		struct spring sp;
		sp.x = s->sy;
		sp.v = s->sv;
		sp.target = s->st;
		spring_run(&sp, n * 10);
		s->sy = sp.x;
		s->sv = sp.v;
		if (s->sy == s->st)
			s->snapping = s->sv = 0;
		return 1;
	}
	if (s->sy < 0 || s->sy > mx) {		/* out of bounds: spring back */
		struct spring sp;
		sp.x = s->sy;
		sp.v = s->sv;
		sp.target = s->sy < 0 ? 0 : mx;
		spring_run(&sp, n * 10);
		s->sy = sp.x;
		s->sv = sp.v;
		return 1;
	}
	if (!s->sv)
		return 0;
	for (; n > 0 && s->sv; n--) {		/* coast */
		s->sy += s->sv / 100;
		s->sv = s->sv * 49 / 50;
		if (s->sv > -2560 && s->sv < 2560)	/* below 10 px/s */
			s->sv = 0;
	}
	return 1;
}

/* rubber band for a drag beyond the edge (px) */
static int rubber(int over, int dim)
{
	int a = over < 0 ? -over : over;

	return over * dim * 55 / (100 * dim + 55 * a);
}

/* page the top screen up (-1) or down (+1): one page, keeping a strip of
 * the previous page for context, snapping in with a spring */
static void page(int dir)
{
	struct screen *s = &stack[depth];
	int pg = content_bottom(s) - content_top(s) - 40, mx = scroll_max(s);
	int t = ((s->snapping ? s->st : s->sy) >> 8) + dir * pg;

	if (s->kind == S_HOME) {
		t = home_page() + dir;
		pager.target = (t < 0 ? 0 : t >= NPAGE ? NPAGE - 1 : t) * PG_STEP << 8;
		return;
	}
	if (t < 0) t = 0;
	if (t > mx) t = mx;
	s->st = t << 8;
	s->snapping = 1;
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

/* store a value in data_srv like sys_srv's httppost: plain text body.
 * data_srv tells its subscribers (SysService, Flash) about the change. */
static void http_post(const char *path, const char *value)
{
	char req[256], rsp[128];
	int fd = tcp("80"), k = 0, n = slen(value);

	if (fd < 0) {
		logs("POST: no data_srv for ", path);
		return;
	}
	k = cat(req, k, "POST ");
	k = cat(req, k, path);
	k = cat(req, k, " HTTP/1.1\r\nUser-Agent: HAO\r\nConnection: close\r\n"
		"Content-Type: text/plain\r\nContent-Length: ");
	k = catnum(req, k, n);
	k = cat(req, k, "\r\n\r\n");
	k = cat(req, k, value);
	write(fd, req, k);
	if (readable(fd, 30))		/* short wait; do not stall the UI */
		read(fd, rsp, sizeof(rsp));
	close(fd);
	logs("POST ", path);
}

static void post_num(const char *path, int v)
{
	char b[12];

	catnum(b, 0, v);
	http_post(path, b);
}

/* value data_srv keeps in its cache file (/usr/local/cache/system_<name>.dat) */
static int cached(const char *name, int def)
{
	char path[80], b[16];
	int fd, n;

	cat(path, cat(path, cat(path, 0, "/usr/local/cache/system_"), name), ".dat");
	fd = open(path, 0);
	if (fd < 0)
		return def;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0 || b[0] < '0' || b[0] > '9')
		return def;
	b[n] = 0;
	return atoi_s(b);
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

/* ---- hardware ---- */

static void tone(int hz, int ms)
{
	int result = 0;

	if (set_sound)
		audio_buzzer_generate_sound(0, hz, ms, &result);
}

static void sound_tick(void) { tone(3200, 4); }
static void sound_ready(void) { tone(2400, 30); tone(3600, 40); }
static void sound_error(void) { tone(400, 120); }

static int battery_logged;

/* battery level 0..8 and charger state from env_mgr (data_srv does not
 * answer GET /system/batterylevel; waiting for it blocked input) */
static void update_power(void)
{
	int result = 0, level = 0, ch = 0;

	env_get_battery_level(&result, &level);
	level &= 0xffff;	/* only 16 bits are written */
	if (result == 0 && level <= 8)
		battery = level;
	result = 0;
	env_get_charger_state(&result, &ch);
	ch &= 0xffff;
	if (battery_logged < 2) {
		lognum("battery level ", level);
		lognum("charger state ", ch);
		battery_logged++;
	}
	if (result == 0)
		charging = ch != 0;
}

static int light_logged;

static void update_sensors(void)
{
	int result = 0, v = 0;

	env_get_cradle_state(&result, &v);
	v &= 0xffff;
	if (result == 0) {
		if (v && !docked)
			dock_dismissed = 0;
		if (docked != (v != 0))
			lognum("cradle ", v);
		docked = v != 0;
	}
	result = v = 0;
	env_get_light_level(&result, &v);
	v &= 0xffff;
	if (result == 0)
		light = v;
	result = v = 0;
	env_get_light_reading(&result, &v);
	v &= 0xffff;
	if (result == 0) {
		if (light_logged < 40 && v != light_raw) {
			lognum("light level ", light);
			lognum("light reading ", v);
			light_logged++;
		}
		light_raw = v;
	}
	if (light_logged == 0) {
		int f = 0;
		result = 0;
		env_get_tune_light_read_freq(&result, &f);
		lognum("light read freq ", f & 0xffff);
		light_logged++;
	}
}

/* in the cradle the bedside clock stays on, dimmed; SysService otherwise
 * turns the display off after the timeout (/system/stayawake keeps it on) */
static void apply_night(int on)
{
	int result = 0;

	if (on == night_on)
		return;
	night_on = on;
	post_num("/system/stayawake", on);
	env_set_backlight_master_level(on ? NIGHT_BRIGHTNESS : set_brightness, &result);
	logs(on ? "night on" : "night off", NULL);
}

/* returns 1 if it had to change the LED (one POST) */
static int set_led(int which, int state)
{
	if (led_state[which] == state)
		return 0;
	led_state[which] = state;
	post_num(led_path[which], state);
	return 1;
}

/* soft-button LEDs show what the buttons do right now */
static void update_leds(void)
{
	struct screen *s = &stack[depth];
	int settled = nav.x == nav.target, mx = scroll_max(s);

	if (!settled)
		return;
	if (gesture == G_SCROLL || s->sv)
		return;			/* not while the list moves */
	if (s->kind == S_HOME) {	/* side arrows: a page that way */
		if (!set_led(LED_UNDER_L, 0) && !set_led(LED_UNDER_R, 0) && !set_led(LED_LEFT, home_page() > 0))
			set_led(LED_RIGHT, home_page() < NPAGE - 1);
		return;
	}
	/* at most one change per loop, so input never waits for data_srv */
	if (set_led(LED_UNDER_L, depth > 0 ? 1 : 0) ||
	    set_led(LED_UNDER_R, 0) ||
	    set_led(LED_LEFT, mx > 0 && (s->sy >> 8) > 0 ? 1 : 0))
		return;
	set_led(LED_RIGHT, mx > 0 && (s->sy >> 8) < mx ? 1 : 0);
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

/* one key or activity as NEC 4F 48 <code> <~code>, sent by the patched hao */
static void send_code(int code)
{
	static const char hex[] = "0123456789ABCDEF";
	char c[40];
	int k = cat(c, 0, "0400F4010101004F48");

	c[k++] = hex[code >> 4];
	c[k++] = hex[code & 15];
	c[k++] = hex[(code ^ 0xff) >> 4];
	c[k++] = hex[(code ^ 0xff) & 15];
	c[k] = 0;
	cat(c, k, "010100");
	send_event("SendCode", "Code", c);
	sent_t = now;
}

/* ticket i, or -1 to end the show. Sent twice: infrared is one-way, and
 * choosing the activity that already runs changes nothing in Home Assistant */
static void kino_select(int i)
{
	logs("kino ", i < 0 ? "Aus" : kino[i].name);
	send_code(i < 0 ? KINO_OFF : kino[i].code);
	send_code(i < 0 ? KINO_OFF : kino[i].code);
	running = i;
}

/* hard key: its scancode goes out; what it does is Home Assistant's business */
static void hard_key(int scan)
{
	send_code(0x80 | scan);
}

static int repeats(int scan)
{
	int i;

	for (i = 0; i < NKEYMAP; i++)
		if (keymap[i].scan == scan)
			return keymap[i].repeat;
	return 0;
}

static int timeout_index(void)
{
	int i;

	for (i = 0; i < NTIMEOUT; i++)
		if (timeouts[i] >= set_timeout)
			return i;
	return NTIMEOUT - 1;
}

static void commit(int kind, int index)
{
	struct screen *s = &stack[depth];

	sound_tick();
	switch (kind) {
	case T_ACT: kino_select(index); break;
	case T_POWER: kino_select(-1); break;
	case T_BACK: pop_to(depth - 1); break;
	case T_DEVICES: push(S_DEVICES, 0); break;
	case T_SETTINGS: push(S_SETTINGS, 0); break;
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
	case T_SEG:
		set_timeout = timeouts[index];
		seg_spring.target = seg_x(index) << 8;
		post_num("/system/backlighttimeout", set_timeout);
		break;
	case T_SOUND:
		set_sound = !set_sound;
		sw_spring.target = set_sound ? 256 << 8 : 0;
		post_num("/system/sound", set_sound);
		sound_tick();
		break;
	}
}

static void hao_message(const char *m, int n)
{
	char name[32], send[16];
	const char *end = m + n;

	copy_tag(name, sizeof(name), m, end, "<Name>", "</Name>");
	if (seq(name, "ReceiverLinkDown")) {
		if (status_color != C_RED)
			sound_error();
		set_status("Blaster offline", C_RED);
	} else if (seq(name, "Queue")) {
		copy_tag(send, sizeof(send), m, end, "<Send>", "</Send>");
		if (seq(send, "Done")) {
			if (status_color == C_YELLOW)
				sound_ready();
			set_status("Ready", C_GREEN);
		} else if ((seq(send, "IR") || seq(send, "RF")) && status_color != C_RED)
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

/* finger velocity in px/s over the last ~80 ms of samples */
static void velocity(int *vx, int *vy)
{
	int newest = (h_n - 1) % NHIST, oldest = newest, i, dt;

	*vx = *vy = 0;
	for (i = 1; i < NHIST && i < h_n; i++) {
		int k = (h_n - 1 - i) % NHIST;
		if (now - h_t[k] > 80)
			break;
		oldest = k;
	}
	dt = (int)(h_t[newest] - h_t[oldest]);
	if (dt <= 0)
		return;
	*vx = (h_x[newest] - h_x[oldest]) * 1000 / dt;
	*vy = (h_y[newest] - h_y[oldest]) * 1000 / dt;
}

static void slider_to(int x)
{
	int v = (x - g_grab - SL_X1) * 100 / (SL_X2 - SL_X1), result = 0;

	if (v < 5) v = 5;
	if (v > 100) v = 100;
	if (v != set_brightness) {
		set_brightness = v;
		env_set_backlight_master_level(v, &result);	/* live */
	}
}

/* touch handling; returns 1 to quit */
static int touch(int x, int y, int down, int was_down)
{
	struct screen *s = &stack[depth];
	int kind, index, vx, vy;

	if (night.target) {			/* night clock: a tap wakes the UI */
		if (down && !was_down) {
			dock_dismissed = 1;
			night.target = 0;
			apply_night(0);
		}
		gesture = G_NONE;
		press_kind = T_NONE;
		return 0;
	}

	if (down) {
		h_x[h_n % NHIST] = x;
		h_y[h_n % NHIST] = y;
		h_t[h_n % NHIST] = now;
		h_n++;
	}
	if (down && !was_down) {			/* touch-down */
		g_x0 = x;
		g_y0 = y;
		g_sy0 = s->sy;
		h_n = 0;
		h_x[0] = x; h_y[0] = y; h_t[0] = now; h_n = 1;
		s->sv = 0;				/* catch a coasting list */
		s->snapping = 0;
		if (depth > 0 && x < EDGE && nav.x == nav.target) {
			gesture = G_EDGE;
			press_kind = T_NONE;
			return 0;
		}
		hit(x, y, 0, &press_kind, &press_index);
		if (press_kind == T_SLIDER) {		/* grab the knob where it is touched */
			int kx = SL_X1 + (SL_X2 - SL_X1) * set_brightness / 100;
			g_grab = x - kx > -16 && x - kx < 16 ? x - kx : 0;
			gesture = G_SLIDER;
			slider_to(x);
			return 0;
		}
		gesture = G_TAP;
		/* in scrollable lists and on the pages the press shows only if the finger stays */
		press_t = (scroll_max(s) > 0 || s->kind == S_HOME) && press_kind != T_POWER && press_kind != T_BACK ?
			  now + TAP_DELAY : now;
		return 0;
	}
	if (down) {					/* move */
		if (gesture == G_TAP) {
			int dy = y - g_y0, dx = x - g_x0;
			if (s->kind == S_HOME && (dx > SLOP || dx < -SLOP) && dx * dx > dy * dy) {
				gesture = G_PAGE;
				press_kind = T_NONE;
				g_x0 = x;		/* start 1:1 from here, no jump */
				g_p0 = pager.x;
			} else if ((dy > SLOP || dy < -SLOP) && scroll_max(s) > 0) {
				gesture = G_SCROLL;
				press_kind = T_NONE;
				g_y0 = y;		/* start 1:1 from here, no jump */
				g_sy0 = s->sy;
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
		} else if (gesture == G_SLIDER) {
			slider_to(x);
		} else if (gesture == G_PAGE) {
			int p = (g_p0 >> 8) - (x - g_x0), mx = (NPAGE - 1) * PG_STEP;
			if (p < 0)
				p = rubber(p, gfx_w);
			else if (p > mx)
				p = mx + rubber(p - mx, gfx_w);
			pager.x = p << 8;
			pager.v = 0;
		}
		return 0;
	}
	/* release */
	velocity(&vx, &vy);
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
		s->sv = -vy * 256;			/* hand the finger's velocity to the list */
	} else if (gesture == G_EDGE) {
		/* project where the swipe would come to rest (deceleration 0.998/ms) */
		int dx = x - g_x0 + vx / 2;
		if (dx > gfx_w / 2)
			pop_to(depth - 1);
		else
			nav.target = depth * gfx_w << 8;
		nav.v = -vx * 256;
	} else if (gesture == G_SLIDER) {
		post_num("/system/backlightlevel", set_brightness);	/* save */
		sound_tick();
	} else if (gesture == G_PAGE) {
		/* the page where the swipe would come to rest, at most one from where it began */
		int from = (g_p0 + (PG_STEP << 7)) / (PG_STEP << 8), rest = (pager.x >> 8) - vx / 2;
		int to = rest < 0 ? 0 : (rest + PG_STEP / 2) / PG_STEP;
		if (to > from + 1) to = from + 1;
		if (to < from - 1) to = from - 1;
		if (to >= NPAGE) to = NPAGE - 1;
		if (to != from)
			sound_tick();
		pager.target = to * PG_STEP << 8;
		pager.v = -vx * 256;
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
	int i, n, held = -1, held_ms = 0, touching = 0, quit = 0, dirty = 1, frames = 0, frame_max = 0;
	long last_frame = 0, last_clock = 0, last_power = 0, last_sensor = 0, last_motion;

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

	logs("ohui v" VERSION " start", NULL);
	now = now_ms();
	curtain_init();
	hao = tcp("1100");
	logs(hao >= 0 ? "hao connected" : "hao NOT connected", NULL);
	if (hao < 0)
		set_status("No connection", C_RED);
	env_service_connect("127.0.0.1");
	audio_service_connect("127.0.0.1");
	set_brightness = cached("backlightlevel", 60);
	set_timeout = cached("backlighttimeout", 30);
	set_sound = cached("sound", 0);
	lognum("settings brightness ", set_brightness);
	lognum("settings timeout ", set_timeout);
	lognum("settings sound ", set_sound);
	seg_spring.x = seg_spring.target = seg_x(timeout_index()) << 8;
	sw_spring.x = sw_spring.target = set_sound ? 256 << 8 : 0;
	update_clock();
	update_power();
	update_sensors();
	stack[0].kind = S_HOME;
	stack[0].shown = now;
	if (gfi_group_attach(&grp, 1, 0) != 0 ||
	    gfi_layer_attach(&il, grp, 0, gfx_main_layer) != 0 ||
	    gfi_layer_set_attrs(il, 0, 0, gfx_w - 1, gfx_h - 1, 7) != 0 ||
	    gfi_set_keyboard_focus(grp, gfx_main_layer) != 0) {
		logs("input setup failed", NULL);
		return 4;
	}
	last_clock = last_power = last_sensor = last_motion = now_ms();

	while (!quit) {
		int key = -1, moving;

		now = now_ms();
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
			if (night.target) {
				dock_dismissed = 1;
				night.target = 0;
				apply_night(0);
			}
			if (key == SCAN_OFF)
				kino_select(-1);
			else if (key == SCAN_ACTIVITIES)
				pop_to(0);
			else if (key == SCAN_UNDER_L)
				pop_to(depth - 1);
			else if (key == SCAN_UNDER_R)
				;
			else if (key == SCAN_SIDE_L)
				page(-1);
			else if (key == SCAN_SIDE_R)
				page(1);
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

		/* 2. hao, clock, power */
		if (hao_poll())
			dirty = 1;
		if (sent_t && now - sent_t >= 400) {	/* lamp goes out */
			sent_t = 0;
			dirty = 1;
		}
		if (now - last_clock > 15000) {
			last_clock = now;
			update_clock();
			dirty = 1;
		}
		if (now - last_sensor > 1000) {
			int d = docked;
			last_sensor = now;
			update_sensors();
			night.target = docked && !dock_dismissed ? 256 << 8 : 0;
			apply_night(night.target != 0);
			if (d != docked || (stack[depth].kind == S_SETTINGS))
				dirty = 1;
		}
		if (now - last_power > 4000) {
			int b = battery, c = charging;
			last_power = now;
			update_power();
			if (b != battery || c != charging)
				dirty = 1;
		}

		/* 3. motion, by real elapsed time in 10 ms steps */
		{
			int steps = (int)(now - last_motion) / 10;
			if (steps > 10)
				steps = 10;		/* after a stall, do not jump */
			if (steps > 0)
				last_motion += steps * 10;
			if (steps < 0)
				last_motion = now, steps = 0;
			moving = 0;
			if (steps) {
				if (gesture != G_EDGE && nav.x != nav.target)
					spring_run(&nav, steps * 10);
				if (gesture != G_PAGE && pager.x != pager.target)
					spring_run(&pager, steps * 10);
				if (seg_spring.x != seg_spring.target)
					spring_run(&seg_spring, steps * 10);
				if (sw_spring.x != sw_spring.target)
					spring_run(&sw_spring, steps * 10);
				if (night.x != night.target)
					spring_run(&night, steps * 10);
				if (scroll_step(&stack[depth], steps))
					moving = 1;
			}
			if (nav.x != nav.target || pager.x != pager.target || seg_spring.x != seg_spring.target ||
			    sw_spring.x != sw_spring.target || night.x != night.target ||
			    stack[depth].snapping || stack[depth].sv)
				moving = 1;
			if (night.x && charging)
				moving = moving || (now - last_frame) > 60;	/* breathing dot */
		}
		settle_stack();
		if (press_kind != T_NONE && gesture == G_TAP && now >= press_t && now < press_t + 20)
			dirty = 1;		/* delayed press becomes visible */
		if (now - stack[depth].shown < 1300)
			moving = 1;		/* scroll indicator flash fades */
		update_leds();

		/* 4. draw: while moving every loop, else at most every FRAME_MS */
		if ((dirty || moving) && (moving || now - last_frame >= FRAME_MS)) {
			long t0 = now_ms();
			render();
			dr_present(0, 0, gfx_w - 1, gfx_h - 1);
			if (frames < 3 || now_ms() - t0 > frame_max) {	/* the slowest frame so far */
				frame_max = (int)(now_ms() - t0);
				lognum("frame ms ", frame_max);
			}
			frames++;
			last_frame = now;
			dirty = 0;
		}
		usleep(POLL_MS * 1000);
	}
	logs("ohui end (classic ui)", NULL);
	apply_night(0);
	for (i = 0; i < 4; i++)
		set_led(i, 0);
	if (hao >= 0)
		close(hao);
	gf_dev_detach(gfx_dev);
	return 0;
}
#endif
