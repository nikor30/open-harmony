/* Harmony 900: native user interface (replaces the Flash UI).
 *
 * Reads the user's activities from data_srv (GET /userconfig) and talks to
 * hao on 127.0.0.1:1100 like the Flash UI does (XML events terminated by a
 * zero byte):
 *   StartActivity <Id>   when an activity (or "power off", Id -1) is tapped
 *   SendIR <ActionId>    for hard keys (activity ControlGroup "HardButtons",
 *                        Button name -> ActionId) and soft buttons (group
 *                        "Misc", shown as a touch grid)
 * hao's Queue/ReceiverLinkDown replies drive the status line.
 *
 * Home: one button per activity, "FLASH UI" at the bottom ends ohui; then
 * flash.sh starts the Flash player as before (also if ohui crashes).
 * Activity: soft buttons, hard keys send their commands (volume, channel and
 * direction keys repeat while held); ACTIVITIES or UNDER LEFT goes home, OFF
 * powers everything off. A small log goes to /usr/local/cache/ohui.log.
 * Built with tools/qnx/qnxcc.py, see README.md. */
#include <ohgfx.h>

#define POLL_MS		20
#define REPEAT_DELAY	400
#define REPEAT_MS	150
#define MAXACT		8
#define MAXHARD		48
#define MAXSOFT		12

#define C_BG		0x000000
#define C_BAR		0x202848
#define C_TITLE		0xffffff
#define C_ACT		0x204080
#define C_OFF		0x900000
#define C_SOFT		0x305030
#define C_SEL		0xc0c000
#define C_DIM		0x404040

struct hard { char name[16]; char action[48]; };
struct soft { char label[16]; char action[48]; int pos; };
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
static int cur = -1;		/* activity shown, -1 = home */
static int hao = -1;
static char rx[8192];
static int rxlen;
static char logbuf[16 * 1024];
static int loglen;

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

/* ---- small string helpers (no libc string functions linked) ---- */

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

/* copy the text between tag and etag (first occurrence in [s, end)) */
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
	set[fd / 32] = 1u << (fd % 32);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select(fd + 1, set, NULL, NULL, &tv) > 0;
}

static int get_userconfig(const char **body)
{
	static const char req[] = "GET /userconfig HTTP/1.1\r\nHost: 127.0.0.1\r\n"
				  "Connection: close\r\n\r\n";
	int fd = tcp("80"), n = 0, r, want = -1;
	const char *hdr_end, *cl;

	if (fd < 0)
		return -1;
	write(fd, req, sizeof(req) - 1);
	while (n < (int)sizeof(cfg) - 1 && readable(fd, 3000)) {
		r = read(fd, cfg + n, sizeof(cfg) - 1 - n);
		if (r <= 0)
			break;
		n += r;
		hdr_end = find(cfg, cfg + n, "\r\n\r\n");
		if (hdr_end && want < 0 && (cl = find(cfg, hdr_end, "Content-Length: "))) {
			want = 0;
			for (cl += 16; *cl >= '0' && *cl <= '9'; cl++)
				want = want * 10 + *cl - '0';
		}
		if (hdr_end && want >= 0 && n - (hdr_end + 4 - cfg) >= want)
			break;
	}
	close(fd);
	cfg[n] = 0;
	hdr_end = find(cfg, cfg + n, "\r\n\r\n");
	if (!hdr_end)
		return -1;
	*body = hdr_end + 4;
	return n - (hdr_end + 4 - cfg);
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
		if (!sb->label[0])	/* no label: use the icon name */
			copy_tag(sb->label, sizeof(sb->label), b, be, "<Icon>", "</Icon>");
		for (k = 0; sb->label[k]; k++)
			if (sb->label[k] == '_')
				sb->label[k] = ' ';
		copy_tag(sb->action, sizeof(sb->action), b, be, "<ActionId>", "</ActionId>");
		copy_tag(pos, sizeof(pos), b, be, "<Position>", "</Position>");
		sb->pos = 0;
		for (k = 0; pos[k] >= '0' && pos[k] <= '9'; k++)
			sb->pos = sb->pos * 10 + pos[k] - '0';
		if (sb->action[0])
			a->nsoft++;
	}
	/* sort soft buttons by position */
	for (k = 1; k < a->nsoft; k++) {
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
	/* power off (Id -1) last */
	for (i = 0; i < nact - 1; i++)
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

/* ---- screens ---- */

#define TOP		28
#define STATUS_Y	(gfx_h - 44)
#define BOTTOM_Y	(gfx_h - 28)

/* home: activity buttons */
#define HB_Y(i)		(TOP + 8 + (i) * 42)
#define HB_H		36
/* activity: soft buttons, 2 columns */
#define SB_X(i)		(6 + ((i) & 1) * 117)
#define SB_Y(i)		(TOP + 8 + ((i) >> 1) * 40)
#define SB_W		111
#define SB_H		34

static char status_text[40];
static unsigned status_rgb = 0x909090;

static void draw_status(void)
{
	gfx_rect(0, STATUS_Y, gfx_w - 1, STATUS_Y + 13, C_BG);
	gfx_text(6, STATUS_Y, status_text, 2, status_rgb, ~0u);
}

static void set_status(const char *s, unsigned rgb)
{
	int k = 0;

	while (*s && k < 19)
		status_text[k++] = *s++;
	status_text[k] = 0;
	status_rgb = rgb;
	gf_draw_begin(gfx_ctx);
	draw_status();
	gf_draw_end(gfx_ctx);
}

static void button(int x1, int y1, int x2, int y2, unsigned bg, const char *label, int scale)
{
	char b[24];
	int k, max = (x2 - x1 - 8) / (6 * scale);

	for (k = 0; label[k] && k < max && k < 23; k++)
		b[k] = label[k];
	b[k] = 0;
	gfx_rect(x1, y1, x2, y2, bg);
	gfx_text(x1 + 6, y1 + (y2 - y1 + 1 - 7 * scale) / 2, b, scale, 0xffffff, ~0u);
}

static void title(const char *s)
{
	gfx_rect(0, 0, gfx_w - 1, TOP - 1, C_BAR);
	gfx_text(6, 7, s, 2, C_TITLE, ~0u);
}

static void draw_home(int sel)
{
	int i;

	gfx_rect(0, 0, gfx_w - 1, gfx_h - 1, C_BG);
	title("ACTIVITIES");
	for (i = 0; i < nact; i++)
		button(6, HB_Y(i), gfx_w - 7, HB_Y(i) + HB_H - 1,
		       i == sel ? C_SEL : act[i].id[0] == '-' ? C_OFF : C_ACT,
		       act[i].label[0] ? act[i].label : act[i].id, 2);
	if (nact == 0)
		gfx_text(6, TOP + 20, "NO ACTIVITIES", 2, 0xff4040, ~0u);
	button(6, BOTTOM_Y, 116, gfx_h - 4, C_DIM, "FLASH UI", 2);
	draw_status();
}

static void draw_activity(int sel)
{
	struct activity *a = &act[cur];
	int i;

	gfx_rect(0, 0, gfx_w - 1, gfx_h - 1, C_BG);
	title(a->label);
	for (i = 0; i < a->nsoft; i++)
		button(SB_X(i), SB_Y(i), SB_X(i) + SB_W - 1, SB_Y(i) + SB_H - 1,
		       i == sel ? C_SEL : C_SOFT, a->soft[i].label, 2);
	button(6, BOTTOM_Y, 116, gfx_h - 4, C_DIM, "HOME", 2);
	if (power_off_index() >= 0)
		button(123, BOTTOM_Y, gfx_w - 7, gfx_h - 4, C_OFF, "AUS", 2);
	draw_status();
}

static void redraw(int sel)
{
	gf_draw_begin(gfx_ctx);
	if (cur < 0)
		draw_home(sel);
	else
		draw_activity(sel);
	gf_draw_end(gfx_ctx);
}

static void start_activity(int i)
{
	logs("start activity ", act[i].label);
	send_event("StartActivity", "Id", act[i].id);
	if (act[i].id[0] == '-') {
		cur = -1;
		set_status("POWER OFF...", 0xffff00);
	} else {
		cur = i;
		set_status("STARTING...", 0xffff00);
	}
	redraw(-1);
}

static void hard_key(int scan)
{
	struct activity *a;
	int i, j;

	if (cur < 0)
		return;
	a = &act[cur];
	for (i = 0; i < NKEYMAP; i++)
		if (keymap[i].scan == scan)
			break;
	if (i == NKEYMAP)
		return;
	for (j = 0; j < a->nhard; j++)
		if (seq(a->hard[j].name, keymap[i].name)) {
			send_event("SendIR", "ActionId", a->hard[j].action);
			set_status(keymap[i].name, 0x00ffff);
			return;
		}
	set_status("NOT MAPPED", 0x909090);
}

static int repeats(int scan)
{
	int i;

	for (i = 0; i < NKEYMAP; i++)
		if (keymap[i].scan == scan)
			return keymap[i].repeat;
	return 0;
}

/* handle one complete message from hao */
static void hao_message(const char *m, int n)
{
	char name[32], send[16], st[16];
	const char *end = m + n;

	copy_tag(name, sizeof(name), m, end, "<Name>", "</Name>");
	if (seq(name, "ReceiverLinkDown")) {
		set_status("BLASTER UNREACHABLE", 0xff4040);
	} else if (seq(name, "Queue")) {
		copy_tag(send, sizeof(send), m, end, "<Send>", "</Send>");
		copy_tag(st, sizeof(st), m, end, "<DeviceStatus>", "</DeviceStatus>");
		if (seq(send, "Done"))
			set_status("DONE", 0x00ff00);
		else if (seq(send, "IR") || seq(send, "RF"))
			set_status(seq(send, "IR") ? "SENDING IR..." : "SENDING RF...", 0xffff00);
		(void)st;
	} else if (name[0]) {
		logs("hao ", name);
	}
}

static void hao_poll(void)
{
	int n, i, start;

	if (hao < 0 || !readable(hao, 0))
		return;
	n = read(hao, rx + rxlen, sizeof(rx) - 1 - rxlen);
	if (n <= 0) {
		logs("hao closed", NULL);
		close(hao);
		hao = -1;
		rxlen = 0;
		return;
	}
	rxlen += n;
	for (start = 0, i = 0; i < rxlen; i++)
		if (rx[i] == 0) {
			hao_message(rx + start, i - start);
			start = i + 1;
		}
	if (start > 0) {		/* keep an incomplete message */
		for (i = start; i < rxlen; i++)
			rx[i - start] = rx[i];
		rxlen -= start;
	}
	if (rxlen >= (int)sizeof(rx) - 1)
		rxlen = 0;
}

int main(int argc, char **argv, char **envp)
{
	unsigned ev[16];
	gfi_group_t grp;
	gfi_layer_t il;
	const char *body;
	int i, n, held = -1, held_ms = 0, touching = 0, quit = 0;

	(void)argc; (void)argv; (void)envp;
	if ((n = gfx_init()) != 0)
		return n;
	gf_draw_begin(gfx_ctx);
	gfx_rect(0, 0, gfx_w - 1, gfx_h - 1, C_BG);
	title("OPEN HARMONY");
	gfx_text(6, 60, "LOADING...", 2, 0xffffff, ~0u);
	gf_draw_end(gfx_ctx);

	logs("ohui start", NULL);
	n = get_userconfig(&body);
	if (n > 0)
		parse_config(body, n);
	for (i = 0; i < nact; i++)
		logs("activity ", act[i].label);
	hao = tcp("1100");
	logs(hao >= 0 ? "hao connected" : "hao NOT connected", NULL);
	if (gfi_group_attach(&grp, 1, 0) != 0 ||
	    gfi_layer_attach(&il, grp, 0, gfx_main_layer) != 0 ||
	    gfi_layer_set_attrs(il, 0, 0, gfx_w - 1, gfx_h - 1, 7) != 0 ||
	    gfi_set_keyboard_focus(grp, gfx_main_layer) != 0) {
		logs("input setup failed", NULL);
		return 4;
	}
	set_status(hao >= 0 ? "READY" : "HAO NOT CONNECTED", hao >= 0 ? 0x00ff00 : 0xff4040);
	redraw(-1);

	while (!quit) {
		int key = -1, tap_x = -1, tap_y = -1;

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
				if (ev[4] && !touching) {
					tap_x = ev[2];
					tap_y = ev[3];
				}
				touching = ev[4] != 0;
			}
			for (i = 0; i < 16; i++)
				ev[i] = 0;
		}

		if (key >= 0) {
			if (key == SCAN_OFF && power_off_index() >= 0) {
				start_activity(power_off_index());
			} else if (key == SCAN_ACTIVITIES || key == SCAN_UNDER_L) {
				cur = -1;
				redraw(-1);
			} else {
				hard_key(key);
			}
		} else if (held >= 0 && cur >= 0 && repeats(held)) {
			held_ms += POLL_MS;
			if (held_ms >= REPEAT_DELAY) {
				hard_key(held);
				held_ms = REPEAT_DELAY - REPEAT_MS;
			}
		}

		if (tap_y >= 0) {
			if (tap_y >= BOTTOM_Y) {
				if (tap_x < 120) {
					if (cur < 0)
						quit = 1;	/* FLASH UI */
					else {
						cur = -1;
						redraw(-1);
					}
				} else if (cur >= 0 && power_off_index() >= 0) {
					start_activity(power_off_index());
				}
			} else if (cur < 0) {
				for (i = 0; i < nact; i++)
					if (tap_y >= HB_Y(i) && tap_y < HB_Y(i) + HB_H) {
						redraw(i);
						start_activity(i);
					}
			} else {
				struct activity *a = &act[cur];
				for (i = 0; i < a->nsoft; i++)
					if (tap_x >= SB_X(i) && tap_x < SB_X(i) + SB_W &&
					    tap_y >= SB_Y(i) && tap_y < SB_Y(i) + SB_H) {
						redraw(i);
						send_event("SendIR", "ActionId", a->soft[i].action);
						set_status(a->soft[i].label, 0x00ffff);
						usleep(150000);
						redraw(-1);
					}
			}
		}

		hao_poll();
		usleep(POLL_MS * 1000);
	}
	logs("ohui end (flash ui)", NULL);
	if (hao >= 0)
		close(hao);
	gf_dev_detach(gfx_dev);
	return 0;
}
