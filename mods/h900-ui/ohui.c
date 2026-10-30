/* Harmony 900: native user interface (replaces the Flash UI).
 *
 * Reads the user's activities from data_srv (GET /userconfig) and talks to
 * hao on 127.0.0.1:1100 like the Flash UI does (XML events terminated by a
 * zero byte):
 *   StartActivity <Id>   when an activity (or power off, Id -1) is tapped
 *   SendIR <ActionId>    for hard keys (activity ControlGroup "HardButtons",
 *                        Button name -> ActionId) and soft buttons (group
 *                        "Misc", shown as a grid of tiles)
 * hao's Queue/ReceiverLinkDown replies drive the status pill.
 *
 * Drawing goes through ohdraw.h (off-screen frame, blit, anti-aliased text,
 * springs). Touch feedback appears on touch-down; a tap commits on release
 * if the finger is still on the control (10 px slop). Screens slide with a
 * critically damped spring; the screen underneath moves at a third of the
 * speed and dims.
 *
 * Home: activities, "Classic UI" ends ohui (flash.sh then starts the Flash
 * player, as it does if ohui fails). Activity: soft buttons, hard keys send
 * their commands (volume, channel, direction and rocker keys repeat while
 * held); ACTIVITIES, UNDER LEFT or the back button go home, OFF powers
 * everything off. Log: /usr/local/cache/ohui.log. */
#include <ohdraw.h>

#define POLL_MS		20
#define REPEAT_DELAY	400
#define REPEAT_MS	150
#define SLOP		10
#define MAXACT		6
#define MAXHARD		48
#define MAXSOFT		8

/* colours (dark appearance) */
#define C_BG		0x000000
#define C_CARD		0x1c1c1e
#define C_CARD_HI	0x2c2c2e
#define C_TILE		0x26262a
#define C_TILE_HI	0x3a3a3e
#define C_TEXT		0xffffff
#define C_TEXT2		0x8e8e93
#define C_ACCENT	0x0a84ff
#define C_GREEN		0x30d158
#define C_YELLOW	0xffd60a
#define C_RED		0xff453a
#define C_RED_BG	0x2c1412
#define C_RED_BG_HI	0x45201c

static const unsigned tints[] = { 0x0a84ff, 0xbf5af2, 0xff9f0a, 0x64d2ff, 0x30d158, 0xff375f };

struct hard { char name[16]; char action[48]; };
struct soft { char label[24]; char action[48]; int pos; };
struct activity {
	char id[16], label[24];
	struct hard hard[MAXHARD];
	int nhard;
	struct soft soft[MAXSOFT];
	int nsoft;
};

static char cfg[192 * 1024];
static struct activity act[MAXACT];
static int nact;
static int running = -1;	/* activity hao last started */
static int shown = 0;		/* activity on the second screen */
static struct spring nav;	/* 0 = home, 240*256 = activity screen */
static int hao = -1;
static char rx[8192];
static int rxlen;
static char logbuf[16 * 1024];
static int loglen;
static char clock_text[8] = "";
static int battery = -1;

/* status pill */
static char status_text[24] = "Ready";
static unsigned status_color = C_TEXT2;

/* touch: control under the finger */
enum { T_NONE, T_ACT, T_POWER, T_CLASSIC, T_BACK, T_SOFT };
static int press_kind = T_NONE, press_index;

/* scancode -> Logitech HardButtons name */
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

/* HTTP GET from data_srv into buf; returns body or NULL */
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
	while (n < size - 1 && readable(fd, 3000)) {
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

static void update_battery(void)
{
	static char buf[1024];
	char v[8];
	const char *b;
	int n = 0, i;

	b = http_get("/system/batterylevel", buf, sizeof(buf), &n);
	if (!b)
		return;
	copy_tag(v, sizeof(v), b, b + n, "<Value>", "</Value>");
	if (!v[0])
		return;
	for (battery = 0, i = 0; v[i] >= '0' && v[i] <= '9'; i++)
		battery = battery * 10 + v[i] - '0';
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
#endif

/* ---- config ---- */

static void parse_group(struct activity *a, const char *s, const char *end)
{
	const char *g, *ge, *b, *be, *q;
	int k;

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
	g = find(s, end, "<ControlGroup name=\"Misc\">");
	ge = g ? find(g, end, "</ControlGroup>") : NULL;
	for (b = g; ge && a->nsoft < MAXSOFT && (b = find(b, ge, "<Button>")); b = be) {
		struct soft *sb = &a->soft[a->nsoft];
		char pos[8];
		be = find(b, ge, "</Button>");
		if (!be)
			break;
		copy_tag(sb->label, sizeof(sb->label), b, be, "<Label>", "</Label>");
		if (!sb->label[0]) {	/* no label: icon name, "teletext_on" -> "Teletext on" */
			copy_tag(sb->label, sizeof(sb->label), b, be, "<Icon>", "</Icon>");
			for (k = 0; sb->label[k]; k++)
				if (sb->label[k] == '_')
					sb->label[k] = ' ';
			if (sb->label[0] >= 'a' && sb->label[0] <= 'z')
				sb->label[0] -= 'a' - 'A';
		}
		/* collapse double spaces */
		for (k = 0; sb->label[k]; k++)
			if (sb->label[k] == ' ' && sb->label[k + 1] == ' ') {
				int j;
				for (j = k; sb->label[j]; j++)
					sb->label[j] = sb->label[j + 1];
				k--;
			}
		copy_tag(sb->action, sizeof(sb->action), b, be, "<ActionId>", "</ActionId>");
		copy_tag(pos, sizeof(pos), b, be, "<Position>", "</Position>");
		sb->pos = 0;
		for (k = 0; pos[k] >= '0' && pos[k] <= '9'; k++)
			sb->pos = sb->pos * 10 + pos[k] - '0';
		if (sb->action[0])
			a->nsoft++;
	}
	for (k = 1; k < a->nsoft; k++) {	/* sort by position */
		int j = k;
		while (j > 0 && a->soft[j - 1].pos > a->soft[j].pos) {
			struct soft t = a->soft[j];
			a->soft[j] = a->soft[j - 1];
			a->soft[j - 1] = t;
			j--;
		}
	}
}

static void parse_config(const char *s, int n)
{
	const char *end = s + n, *a, *e;
	int i;

	while (nact < MAXACT && (a = find(s, end, "<Activity>")) && (e = find(a, end, "</Activity>"))) {
		struct activity *x = &act[nact];
		copy_tag(x->id, sizeof(x->id), a, e, "<Id>", "</Id>");
		copy_tag(x->label, sizeof(x->label), a, e, "<Label>", "</Label>");
		parse_group(x, a, e);
		if (x->id[0])
			nact++;
		s = e;
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

/* ---- layout (240 x 320) ---- */

#define STATUS_H	20
#define M		12		/* side margin */
#define CARD_Y(i)	(70 + (i) * 62)
#define CARD_H		54
#define POWER_Y		(gfx_h - 52)
#define POWER_H		40
#define CLASSIC_Y	(POWER_Y - 30)
#define BACK_W		110
#define TILE_W		((gfx_w - 2 * M - 8) / 2)
#define TILE_H		38
#define TILE_X(i)	(M + ((i) & 1) * (TILE_W + 8))
#define TILE_Y(i)	(86 + ((i) >> 1) * (TILE_H + 6))

static int in(int x, int y, int x1, int y1, int x2, int y2, int slop)
{
	return x >= x1 - slop && x <= x2 + slop && y >= y1 - slop && y <= y2 + slop;
}

/* control under (x, y) on the visible screen */
static void hit(int x, int y, int slop, int *kind, int *index)
{
	int i;

	*kind = T_NONE;
	if (in(x, y, M, POWER_Y, gfx_w - M, POWER_Y + POWER_H, slop) && power_off_index() >= 0) {
		*kind = T_POWER;
		return;
	}
	if (nav.target == 0) {
		for (i = 0; i < n_activities(); i++)
			if (in(x, y, M, CARD_Y(i), gfx_w - M, CARD_Y(i) + CARD_H, slop)) {
				*kind = T_ACT;
				*index = i;
				return;
			}
		if (in(x, y, 60, CLASSIC_Y, gfx_w - 60, CLASSIC_Y + 22, slop))
			*kind = T_CLASSIC;
		return;
	}
	if (in(x, y, 0, STATUS_H, BACK_W, STATUS_H + 26, slop)) {
		*kind = T_BACK;
		return;
	}
	for (i = 0; i < act[shown].nsoft; i++)
		if (in(x, y, TILE_X(i), TILE_Y(i), TILE_X(i) + TILE_W - 1, TILE_Y(i) + TILE_H - 1, slop)) {
			*kind = T_SOFT;
			*index = i;
			return;
		}
}

/* ---- drawing ---- */

static void draw_background(void)
{
	dr_fill(0, 0, gfx_w - 1, gfx_h - 1, C_BG, 256);
}

static void draw_status_bar(void)
{
	int x;

	dr_text(&font_small_medium, M, 14, clock_text, 0, C_TEXT, 256);
	if (battery >= 0) {
		char pct[8];
		int k = 0, fillw, lvl = battery > 100 ? 100 : battery;
		unsigned c = lvl <= 20 ? C_RED : C_TEXT;
		if (lvl >= 100) pct[k++] = '1';
		if (lvl >= 10) pct[k++] = '0' + (lvl / 10) % 10;
		pct[k++] = '0' + lvl % 10;
		pct[k++] = '%';
		pct[k] = 0;
		x = gfx_w - M - 24;
		dr_rrect(x, 5, x + 21, 15, 3, C_TEXT, 90);	/* battery outline */
		dr_rrect(x + 1, 6, x + 20, 14, 2, C_BG, 256);
		fillw = 18 * lvl / 100;
		if (fillw > 0)
			dr_rrect(x + 2, 7, x + 1 + fillw, 13, 2, c, 256);
		dr_fill(x + 22, 8, x + 23, 12, C_TEXT, 90);
		dr_text(&font_caption, x - 4 - dr_text_width(&font_caption, pct, 0), 14, pct, 0, C_TEXT2, 256);
	}
}

static void draw_power_button(void)
{
	int pressed = press_kind == T_POWER;
	const char *t = "Power Off";
	int w = dr_text_width(&font_button, t, 0);

	dr_rrect(M, POWER_Y, gfx_w - 1 - M, POWER_Y + POWER_H - 1, 12,
		 pressed ? C_RED_BG_HI : C_RED_BG, 256);
	dr_text(&font_button, (gfx_w - w) / 2, POWER_Y + 26, t, 0, C_RED, 256);
}

static void draw_home(void)
{
	int i;

	draw_background();
	dr_text(&font_title, M, 56, "Activities", -1, C_TEXT, 256);
	for (i = 0; i < n_activities(); i++) {
		int y = CARD_Y(i), pressed = press_kind == T_ACT && press_index == i;
		unsigned tint = tints[i % 6];
		char initial[4];
		const char *sub = i == running ? "Running" : "Tap to start";

		dr_shadow(M, y, gfx_w - 1 - M, y + CARD_H - 1, 14, 2, 70);
		dr_rrect(M, y, gfx_w - 1 - M, y + CARD_H - 1, 14, pressed ? C_CARD_HI : C_CARD, 256);
		dr_hspan(M + 14, gfx_w - 1 - M - 14, y, 0xffffff, 18);	/* top highlight */
		/* icon: tinted circle with the initial */
		dr_rrect(M + 10, y + 10, M + 43, y + 43, 17, tint, 256);
		initial[0] = act[i].label[0] ? act[i].label[0] : '?';
		initial[1] = 0;
		if ((unsigned char)initial[0] == 0xc3) {
			initial[1] = act[i].label[1];
			initial[2] = 0;
		}
		dr_text(&font_headline, M + 27 - dr_text_width(&font_headline, initial, 0) / 2,
			y + 33, initial, 0, C_TEXT, 256);
		dr_text_fit(&font_button, M + 54, y + 24, act[i].label, 0, C_TEXT, 256, gfx_w - 2 * M - 80);
		if (i == running)
			dr_rrect(M + 54, y + 34, M + 59, y + 39, 3, C_GREEN, 256);
		dr_text(&font_caption, M + 54 + (i == running ? 9 : 0), y + 41, sub, 0,
			i == running ? C_GREEN : C_TEXT2, 256);
		/* chevron */
		dr_text(&font_headline, gfx_w - M - 20, y + 34, ">", 0, C_TEXT2, 200);
	}
	if (n_activities() == 0)
		dr_text(&font_body, M, 100, "No activities found", 0, C_TEXT2, 256);
	{
		const char *t = "Classic UI";
		int w = dr_text_width(&font_body, t, 0);
		dr_text(&font_body, (gfx_w - w) / 2, CLASSIC_Y + 16, t, 0,
			press_kind == T_CLASSIC ? C_TEXT : C_TEXT2, 256);
	}
	draw_power_button();
}

static void draw_activity(void)
{
	struct activity *a = &act[shown];
	int i, w;

	draw_background();
	/* back button */
	dr_text(&font_body, M - 2, STATUS_H + 18, "<", 0, C_ACCENT, press_kind == T_BACK ? 120 : 256);
	dr_text(&font_body, M + 10, STATUS_H + 18, "Activities", 0, C_ACCENT,
		press_kind == T_BACK ? 120 : 256);
	/* status pill, right */
	w = dr_text_width(&font_caption, status_text, 0) + 22;
	dr_rrect(gfx_w - M - w, STATUS_H + 4, gfx_w - M - 1, STATUS_H + 22, 9, status_color, 46);
	dr_rrect(gfx_w - M - w + 8, STATUS_H + 10, gfx_w - M - w + 13, STATUS_H + 15, 3, status_color, 256);
	dr_text(&font_caption, gfx_w - M - w + 17, STATUS_H + 17, status_text, 0, status_color, 256);
	/* title */
	dr_text_fit(&font_title, M, 74, a->label, -1, C_TEXT, 256, gfx_w - 2 * M);
	/* soft buttons */
	for (i = 0; i < a->nsoft; i++) {
		int x = TILE_X(i), y = TILE_Y(i), pressed = press_kind == T_SOFT && press_index == i;
		int inset = pressed ? 1 : 0;
		dr_rrect(x + inset, y + inset, x + TILE_W - 1 - inset, y + TILE_H - 1 - inset, 11,
			 pressed ? C_TILE_HI : C_TILE, 256);
		dr_hspan(x + 11, x + TILE_W - 12, y + inset, 0xffffff, 16);
		w = dr_text_width(&font_body, a->soft[i].label, 0);
		if (w > TILE_W - 12)
			dr_text_fit(&font_body, x + 6, y + 24, a->soft[i].label, 0, C_TEXT, 256, TILE_W - 12);
		else
			dr_text(&font_body, x + (TILE_W - w) / 2, y + 24, a->soft[i].label, 0, C_TEXT, 256);
	}
	if (a->nsoft == 0)
		dr_text(&font_body, M, 110, "Use the keys to control", 0, C_TEXT2, 256);
	draw_power_button();
}

/* full frame for the current navigation position */
static void render(void)
{
	int p = nav.x >> 8;	/* 0 home .. gfx_w activity */

	dr_clip(0, 0, gfx_w - 1, gfx_h - 1);
	if (p <= 0) {
		dr_ox = 0;
		draw_home();
	} else if (p >= gfx_w) {
		dr_ox = 0;
		draw_activity();
	} else {
		/* home moves left at a third of the speed and dims; activity slides over it */
		dr_ox = -p / 3;
		dr_clip(0, 0, gfx_w - p - 1, gfx_h - 1);
		draw_home();
		dr_ox = 0;
		dr_fill(0, 0, gfx_w - p - 1, gfx_h - 1, 0x000000, p * 120 / gfx_w);
		dr_ox = gfx_w - p;
		dr_clip(gfx_w - p, 0, gfx_w - 1, gfx_h - 1);
		draw_activity();
		dr_ox = 0;
		dr_clip(0, 0, gfx_w - 1, gfx_h - 1);
		dr_fill(gfx_w - p - 3, 0, gfx_w - p - 1, gfx_h - 1, 0x000000, 60);	/* edge shadow */
	}
	dr_ox = 0;
	dr_clip(0, 0, gfx_w - 1, gfx_h - 1);
	dr_fill(0, 0, gfx_w - 1, STATUS_H - 1, C_BG, 256);
	draw_status_bar();
}

#ifndef OHUI_HOST
/* ---- actions ---- */

static void set_status(const char *s, unsigned c)
{
	scopy(status_text, s, sizeof(status_text));
	status_color = c;
}

static void go(int screen)
{
	nav.target = screen ? gfx_w << 8 : 0;
}

static void start_activity(int i)
{
	if (i == running) {	/* already on: just show it */
		shown = i;
		go(1);
		return;
	}
	logs("start activity ", act[i].label);
	send_event("StartActivity", "Id", act[i].id);
	if (act[i].id[0] == '-') {
		running = -1;
		set_status("Powering off", C_YELLOW);
		go(0);
	} else {
		running = i;
		shown = i;
		set_status("Starting", C_YELLOW);
		go(1);
	}
}

static void hard_key(int scan)
{
	struct activity *a;
	int i, j;

	if (running < 0)
		return;
	a = &act[running];
	for (i = 0; i < NKEYMAP; i++)
		if (keymap[i].scan == scan)
			break;
	if (i == NKEYMAP)
		return;
	for (j = 0; j < a->nhard; j++)
		if (seq(a->hard[j].name, keymap[i].name)) {
			send_event("SendIR", "ActionId", a->hard[j].action);
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

static void hao_message(const char *m, int n)
{
	char name[32], send[16];
	const char *end = m + n;

	copy_tag(name, sizeof(name), m, end, "<Name>", "</Name>");
	if (seq(name, "ReceiverLinkDown")) {
		set_status("Blaster unreachable", C_RED);
	} else if (seq(name, "Queue")) {
		copy_tag(send, sizeof(send), m, end, "<Send>", "</Send>");
		if (seq(send, "Done"))
			set_status("Ready", C_GREEN);
		else if (seq(send, "IR") || seq(send, "RF"))
			set_status("Sending", C_YELLOW);
	} else if (name[0]) {
		logs("hao ", name);
	}
}

/* returns 1 if anything arrived */
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

int main(int argc, char **argv, char **envp)
{
	unsigned ev[16];
	gfi_group_t grp;
	gfi_layer_t il;
	const char *body;
	int i, n, held = -1, held_ms = 0, touching = 0, quit = 0, dirty = 1, tick = 0;

	(void)argc; (void)argv; (void)envp;
	if ((n = gfx_init()) != 0 || (n = dr_init()) != 0) {
		char b[4] = { '0' + n / 10, '0' + n % 10, 0, 0 };
		logs("graphics init failed at step ", b);
		return n;
	}
	draw_background();
	dr_text(&font_title, M, 56, "Harmony", -1, C_TEXT, 256);
	dr_text(&font_body, M, 84, "Loading...", 0, C_TEXT2, 256);
	dr_present(0, 0, gfx_w - 1, gfx_h - 1);

	logs("ohui start", NULL);
	body = http_get("/userconfig", cfg, sizeof(cfg), &n);
	if (body && n > 0)
		parse_config(body, n);
	for (i = 0; i < nact; i++)
		logs("activity ", act[i].label);
	hao = tcp("1100");
	logs(hao >= 0 ? "hao connected" : "hao NOT connected", NULL);
	if (hao < 0)
		set_status("No connection", C_RED);
	update_clock();
	update_battery();
	if (gfi_group_attach(&grp, 1, 0) != 0 ||
	    gfi_layer_attach(&il, grp, 0, gfx_main_layer) != 0 ||
	    gfi_layer_set_attrs(il, 0, 0, gfx_w - 1, gfx_h - 1, 7) != 0 ||
	    gfi_set_keyboard_focus(grp, gfx_main_layer) != 0) {
		logs("input setup failed", NULL);
		return 4;
	}

	while (!quit) {
		int key = -1;

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
				int x = ev[2], y = ev[3], down = ev[4] != 0, kind, index = 0;
				if (down && !touching) {		/* touch-down: highlight now */
					hit(x, y, 0, &press_kind, &press_index);
					dirty = 1;
				} else if (down && press_kind != T_NONE) {	/* moved: keep or drop */
					hit(x, y, SLOP, &kind, &index);
					if (kind != press_kind || index != press_index) {
						press_kind = T_NONE;
						dirty = 1;
					}
				} else if (!down && touching && press_kind != T_NONE) {	/* release: commit */
					switch (press_kind) {
					case T_ACT: start_activity(press_index); break;
					case T_POWER: start_activity(power_off_index()); break;
					case T_BACK: go(0); break;
					case T_CLASSIC: quit = 1; break;
					case T_SOFT:
						send_event("SendIR", "ActionId", act[shown].soft[press_index].action);
						set_status(act[shown].soft[press_index].label, C_ACCENT);
						break;
					}
					press_kind = T_NONE;
					dirty = 1;
				}
				touching = down;
			}
			for (i = 0; i < 16; i++)
				ev[i] = 0;
		}

		if (key >= 0) {
			dirty = 1;
			if (key == SCAN_OFF && power_off_index() >= 0)
				start_activity(power_off_index());
			else if (key == SCAN_ACTIVITIES || key == SCAN_UNDER_L)
				go(0);
			else
				hard_key(key);
		} else if (held >= 0 && running >= 0 && repeats(held)) {
			held_ms += POLL_MS;
			if (held_ms >= REPEAT_DELAY) {
				hard_key(held);
				held_ms = REPEAT_DELAY - REPEAT_MS;
				dirty = 1;
			}
		}

		if (hao_poll())
			dirty = 1;
		if (++tick >= 1500) {	/* every 30 s */
			tick = 0;
			update_clock();
			update_battery();
			dirty = 1;
		}
		if (nav.x != nav.target) {
			spring_step(&nav);
			dirty = 1;
		}
		if (dirty) {
			render();
			dr_present(0, 0, gfx_w - 1, gfx_h - 1);
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
