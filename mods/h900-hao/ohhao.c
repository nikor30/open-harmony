/* Harmony 900: first native control program. Loads the user's activities
 * from data_srv (HTTP GET /userconfig on 127.0.0.1:80), shows one touch
 * button per activity (power off = Id -1 in red at the bottom) and sends
 * the same message as the Flash UI to hao (127.0.0.1:1100, XML terminated
 * by a zero byte):
 *   <Event><Payload><Name>StartActivity</Name><Params><Id>..</Id></Params></Payload></Event>
 * hao's replies are shown and logged. Hard keys are shown by name
 * (h900keys.h); EXIT ends the program, then flash.sh starts the Flash UI
 * (also after 5 minutes). Everything is logged to /usr/local/cache/ohhao.log
 * (region 12, read back with zregion 12 in safe mode). */
#include <ohgfx.h>
#include <h900keys.h>

#define RUN_MS		300000
#define POLL_MS		20
#define MAXACT		6
#define SCAN_EXIT	0x21

static char cfg[192 * 1024];
static char logbuf[24 * 1024];
static int loglen;
static struct { char id[16]; char label[24]; } act[MAXACT];
static int nact;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

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

static void logs(const char *s, int n)
{
	int fd;

	if (n < 0)
		n = slen(s);
	if (loglen + n + 1 > (int)sizeof(logbuf))
		return;
	while (n-- > 0)
		logbuf[loglen++] = *s ? *s : '|', s++;
	logbuf[loglen++] = '\n';
	fd = creat("/usr/local/cache/ohhao.log", 0644);
	if (fd >= 0) {
		write(fd, logbuf, loglen);
		close(fd);
	}
}

static void lognum(const char *what, unsigned v)
{
	char b[48];
	int k = 0;

	while (*what)
		b[k++] = *what++;
	gfx_hex(b + k, v, 8);
	logs(b, k + 8);
}

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

/* wait up to ms for data on fd */
static int readable(int fd, int ms)
{
	unsigned set[8] = {0};
	struct timeval tv;

	set[fd / 32] = 1u << (fd % 32);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select(fd + 1, set, NULL, NULL, &tv) > 0;
}

/* GET /userconfig; returns body length or -1 */
static int get_userconfig(const char **body)
{
	static const char req[] = "GET /userconfig HTTP/1.1\r\nHost: 127.0.0.1\r\n"
				  "Connection: close\r\n\r\n";
	int fd = tcp("80"), n = 0, r;
	const char *hdr_end, *cl;
	int want = -1;

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

static void copy_tag(char *dst, int max, const char *s, const char *end, const char *tag, const char *etag)
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

static void parse_activities(const char *s, int n)
{
	const char *end = s + n, *a, *e;
	int off = -1, i;

	while (nact < MAXACT && (a = find(s, end, "<Activity>")) && (e = find(a, end, "</Activity>"))) {
		copy_tag(act[nact].id, sizeof(act[0].id), a, e, "<Id>", "</Id>");
		copy_tag(act[nact].label, sizeof(act[0].label), a, e, "<Label>", "</Label>");
		if (act[nact].id[0])
			nact++;
		s = e;
	}
	/* power off (Id -1) last */
	for (i = 0; i < nact; i++)
		if (act[i].id[0] == '-')
			off = i;
	if (off >= 0 && off != nact - 1) {
		char id[16], label[24];
		for (i = 0; i < 16; i++) id[i] = act[off].id[i];
		for (i = 0; i < 24; i++) label[i] = act[off].label[i];
		for (i = off; i < nact - 1; i++)
			act[i] = act[i + 1];
		for (i = 0; i < 16; i++) act[nact - 1].id[i] = id[i];
		for (i = 0; i < 24; i++) act[nact - 1].label[i] = label[i];
	}
}

#define BTN_Y(i)	(40 + (i) * 40)
#define BTN_H		32

static void draw_buttons(int sel)
{
	int i;

	for (i = 0; i < nact; i++) {
		unsigned bg = act[i].id[0] == '-' ? 0x900000 : 0x204080;
		if (i == sel)
			bg = 0xc0c000;
		gfx_rect(8, BTN_Y(i), gfx_w - 9, BTN_Y(i) + BTN_H - 1, bg);
		gfx_text(18, BTN_Y(i) + 8, act[i].label[0] ? act[i].label : act[i].id, 2,
			 0xffffff, ~0u);
	}
}

static void status(int y, const char *s, unsigned rgb)
{
	char b[40];
	int k = 0;

	while (*s && k < 38)
		b[k++] = *s++;
	b[k] = 0;
	gfx_rect(0, y, gfx_w - 1, y + 9, 0x000000);
	gfx_text(4, y, b, 1, rgb, ~0u);
}

int main(int argc, char **argv, char **envp)
{
	unsigned ev[16];
	gfi_group_t grp;
	gfi_layer_t il;
	const char *body;
	char msg[160], rx[2048], line[64];
	int i, ms, ret, n, hao = -1, held = -1, touching = 0, rxlen = 0;

	(void)argc; (void)argv; (void)envp;
	alarm(RUN_MS / 1000 + 10);
	if ((ret = gfx_init()) != 0)
		return ret;
	gf_draw_begin(gfx_ctx);
	gfx_rect(0, 0, gfx_w - 1, gfx_h - 1, 0x000000);
	gfx_text(6, 8, "OPEN HARMONY", 2, 0x8080ff, ~0u);
	gfx_text(6, 60, "LOADING CONFIG...", 2, 0xffffff, ~0u);
	gf_draw_end(gfx_ctx);

	logs("ohhao start", -1);
	n = get_userconfig(&body);
	lognum("userconfig bytes ", n);
	if (n > 0)
		parse_activities(body, n);
	for (i = 0; i < nact; i++) {
		int k = 0;
		const char *p;
		for (p = "activity "; *p; p++) line[k++] = *p;
		for (p = act[i].id; *p; p++) line[k++] = *p;
		line[k++] = ' ';
		for (p = act[i].label; *p; p++) line[k++] = *p;
		logs(line, k);
	}
	hao = tcp("1100");
	lognum("hao connect fd ", hao);

	gf_draw_begin(gfx_ctx);
	gfx_rect(0, 30, gfx_w - 1, gfx_h - 1, 0x000000);
	if (nact == 0)
		gfx_text(6, 60, "NO ACTIVITIES", 2, 0xff4040, ~0u);
	draw_buttons(-1);
	status(gfx_h - 50, hao >= 0 ? "HAO CONNECTED" : "HAO NOT CONNECTED", hao >= 0 ? 0x00ff00 : 0xff4040);
	status(gfx_h - 20, "EXIT KEY: BACK TO NORMAL UI", 0x909090);
	gf_draw_end(gfx_ctx);

	if (gfi_group_attach(&grp, 1, 0) != 0 ||
	    gfi_layer_attach(&il, grp, 0, gfx_main_layer) != 0 ||
	    gfi_layer_set_attrs(il, 0, 0, gfx_w - 1, gfx_h - 1, 7) != 0 ||
	    gfi_set_keyboard_focus(grp, gfx_main_layer) != 0) {
		logs("input setup failed", -1);
		sleep(10);
		goto out;
	}

	for (ms = 0; ms < RUN_MS; ms += POLL_MS) {
		int tap = -1, key = -1;

		for (i = 0; i < 16; i++)
			ev[i] = 0;
		while (gfi_layer_get_event(&il, 1, ev, GFI_NONBLOCK) == 0) {
			if (ev[0] == GFI_EV_KEY) {
				int sc = ev[5] & 0x7f;
				if ((ev[6] & 1) && !(ev[5] & 0x80)) {
					if (sc != held)
						key = sc;
					held = sc;
				} else if (sc == held) {
					held = -1;
				}
			} else if (ev[0] == GFI_EV_POINTER) {
				int y = ev[3];
				if (ev[4] && !touching)
					for (i = 0; i < nact; i++)
						if (y >= BTN_Y(i) && y < BTN_Y(i) + BTN_H)
							tap = i;
				touching = ev[4] != 0;
			}
			for (i = 0; i < 16; i++)
				ev[i] = 0;
		}

		gf_draw_begin(gfx_ctx);
		if (key >= 0) {
			const char *name = h900_key_name(key);
			int k = 0;
			const char *p;
			for (p = "KEY "; *p; p++) line[k++] = *p;
			gfx_hex(line + k, key, 2);
			k += 2;
			line[k++] = ' ';
			for (p = name ? name : "?"; *p; p++) line[k++] = *p;
			line[k] = 0;
			gfx_rect(0, gfx_h - 36, gfx_w - 1, gfx_h - 21, 0x000000);
			gfx_text(4, gfx_h - 36, line, 2, 0xffff00, ~0u);
			logs(line, k);
		}
		if (tap >= 0) {
			const char *p;
			int k = 0;
			draw_buttons(tap);
			for (p = "<Event><Payload><Name>StartActivity</Name><Params><Id>"; *p; p++) msg[k++] = *p;
			for (p = act[tap].id; *p; p++) msg[k++] = *p;
			for (p = "</Id></Params></Payload></Event>"; *p; p++) msg[k++] = *p;
			msg[k++] = 0;
			if (hao < 0)
				hao = tcp("1100");
			ret = hao >= 0 ? write(hao, msg, k) : -1;
			logs("send:", -1);
			logs(msg, k - 1);
			lognum("write ", ret);
			k = 0;
			for (p = ret > 0 ? "SENT: " : "SEND FAILED: "; *p; p++) line[k++] = *p;
			for (p = act[tap].label; *p; p++) line[k++] = *p;
			line[k] = 0;
			status(gfx_h - 64, line, ret > 0 ? 0x00ff00 : 0xff4040);
		}
		if (hao >= 0 && readable(hao, 0)) {
			n = read(hao, rx + rxlen, sizeof(rx) - 1 - rxlen);
			if (n <= 0) {
				logs("hao closed", -1);
				close(hao);
				hao = -1;
				status(gfx_h - 50, "HAO DISCONNECTED", 0xff4040);
			} else {
				char name[40];
				rxlen += n;
				logs("recv:", -1);
				logs(rx, rxlen);
				rx[rxlen] = 0;
				copy_tag(name, sizeof(name), rx, rx + rxlen, "<Name>", "</Name>");
				status(gfx_h - 50, name[0] ? name : "HAO REPLY", 0x00ffff);
				rxlen = 0;
			}
		}
		i = (RUN_MS - ms) * (gfx_w - 1) / RUN_MS;	/* remaining time */
		gfx_rect(0, gfx_h - 4, i, gfx_h - 1, 0x00c000);
		gfx_rect(i + 1, gfx_h - 4, gfx_w - 1, gfx_h - 1, 0x000000);
		gf_draw_end(gfx_ctx);
		if (key == SCAN_EXIT)
			break;
		usleep(POLL_MS * 1000);
	}
out:
	logs("ohhao end", -1);
	if (hao >= 0)
		close(hao);
	gf_dev_detach(gfx_dev);
	return 0;
}
