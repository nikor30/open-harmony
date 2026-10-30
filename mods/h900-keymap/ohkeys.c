/* Harmony 900: guided key mapping, first native program with text.
 * Asks for each key by name ("PRESS: VOL +"), records scancode, cap and sym
 * of the press (repeats ignored until release), a tap on the screen skips a
 * key. After every key the whole table is written to /usr/local/ohkeys.txt
 * (region 12, read back with zwrite in safe mode), lines "NAME<TAB>SCAN
 * <TAB>CAP<TAB>SYM" in hex, SCAN "--" when skipped. Ends when all keys are
 * done (after 5 s) or after 4 minutes; then flash.sh starts the Flash UI.
 * Built with tools/qnx/qnxcc.py, see README.md. */
#include <ohgfx.h>

#define RUN_MS		240000
#define POLL_MS		20

static const char *const names[] = {
	"SIDE LEFT", "SIDE RIGHT", "UNDER LEFT", "UNDER RIGHT",
	"ACTIVITIES", "HELP", "MENU", "ROCKER UP", "INFO", "EXIT", "ROCKER DOWN", "GUIDE",
	"RED", "GREEN", "YELLOW", "BLUE",
	"VOL +", "VOL -", "CH +", "CH -",
	"UP", "LEFT", "OK", "RIGHT", "DOWN", "MUTE", "BACK",
	"REWIND", "PLAY", "FORWARD", "REPLAY", "SKIP", "RECORD", "PAUSE", "STOP",
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "CLEAR", "0", "ENTER",
};
#define NKEYS ((int)(sizeof(names) / sizeof(names[0])))

static int scan[NKEYS], cap[NKEYS], sym[NKEYS];
static const char *path = "/usr/local/ohkeys.txt";
static int file_ok;

static int save(int n)
{
	static char buf[NKEYS * 32];
	int i, len = 0, fd;
	const char *p;

	for (i = 0; i < n; i++) {
		for (p = names[i]; *p; p++)
			buf[len++] = *p;
		buf[len++] = '\t';
		if (scan[i] < 0) {
			buf[len++] = '-';
			buf[len++] = '-';
		} else {
			gfx_hex(buf + len, scan[i], 2);
			len += 2;
		}
		buf[len++] = '\t';
		gfx_hex(buf + len, cap[i], 4);
		len += 4;
		buf[len++] = '\t';
		gfx_hex(buf + len, sym[i], 4);
		len += 4;
		buf[len++] = '\n';
	}
	fd = creat(path, 0644);
	if (fd < 0) {
		path = "/usr/local/cache/ohkeys.txt";
		fd = creat(path, 0644);
	}
	if (fd < 0)
		return 0;
	i = write(fd, buf, len);
	close(fd);
	return i == len;
}

static void screen(int idx, int last)
{
	char num[8], line[40];
	int k = 0;

	gfx_rect(0, 0, gfx_w - 1, gfx_h - 9, 0x000020);
	gfx_text(6, 6, "KEY MAP", 2, 0x8080ff, ~0u);
	if (idx < NKEYS) {
		gfx_text(6, 50, "PRESS:", 2, 0xffffff, ~0u);
		gfx_text(6, 76, names[idx], 3, 0xffff00, ~0u);
	} else {
		gfx_text(6, 70, "DONE", 4, 0x00ff00, ~0u);
	}
	num[0] = '0' + (idx + 1 > NKEYS ? NKEYS : idx + 1) / 10;
	num[1] = '0' + (idx + 1 > NKEYS ? NKEYS : idx + 1) % 10;
	num[2] = '/';
	num[3] = '0' + NKEYS / 10;
	num[4] = '0' + NKEYS % 10;
	num[5] = 0;
	gfx_text(6, 116, num, 2, 0xffffff, ~0u);
	gfx_text(6, 150, "TAP SCREEN TO SKIP", 2, 0x909090, ~0u);
	if (last >= 0) {
		const char *p;
		for (p = names[last]; *p; p++)
			line[k++] = *p;
		line[k++] = ' ';
		if (scan[last] < 0) {
			line[k++] = '-';
			line[k++] = '-';
		} else {
			gfx_hex(line + k, scan[last], 2);
			k += 2;
		}
		line[k] = 0;
		gfx_text(6, 190, line, 2, 0x00ffff, ~0u);
		gfx_text(6, 220, file_ok ? "FILE OK" : "FILE ERROR", 2,
			 file_ok ? 0x00ff00 : 0xff4040, ~0u);
		gfx_text(6, 244, path, 1, 0x909090, ~0u);
	}
}

int main(int argc, char **argv, char **envp)
{
	unsigned ev[16];
	gfi_group_t grp;
	gfi_layer_t il;
	int i, ms, ret, idx = 0, held = -1, touching = 0, done_ms = -1;

	(void)argc; (void)argv; (void)envp;
	alarm(RUN_MS / 1000 + 10);
	if ((ret = gfx_init()) != 0)
		return ret;
	gf_draw_begin(gfx_ctx);
	gfx_rect(0, 0, gfx_w - 1, gfx_h - 1, 0x000000);
	screen(0, -1);
	gf_draw_end(gfx_ctx);
	if (gfi_group_attach(&grp, 1, 0) != 0 ||
	    gfi_layer_attach(&il, grp, 0, gfx_main_layer) != 0 ||
	    gfi_layer_set_attrs(il, 0, 0, gfx_w - 1, gfx_h - 1, 7) != 0 ||
	    gfi_set_keyboard_focus(grp, gfx_main_layer) != 0) {
		gf_draw_begin(gfx_ctx);
		gfx_text(6, 200, "INPUT SETUP FAILED", 2, 0xff4040, ~0u);
		gf_draw_end(gfx_ctx);
		sleep(10);
		goto out;
	}

	for (ms = 0; ms < RUN_MS; ms += POLL_MS) {
		int last = -1;

		for (i = 0; i < 16; i++)
			ev[i] = 0;
		while (gfi_layer_get_event(&il, 1, ev, GFI_NONBLOCK) == 0) {
			if (ev[0] == GFI_EV_KEY) {
				int sc = ev[5] & 0x7f;
				if ((ev[6] & 1) && !(ev[5] & 0x80)) {
					if (sc != held && idx < NKEYS) {
						scan[idx] = sc;
						cap[idx] = ev[4];
						sym[idx] = ev[3];
						last = idx++;
					}
					held = sc;
				} else if (sc == held) {
					held = -1;
				}
			} else if (ev[0] == GFI_EV_POINTER) {
				if (ev[4] && !touching && idx < NKEYS) {
					scan[idx] = -1;
					cap[idx] = sym[idx] = 0;
					last = idx++;
				}
				touching = ev[4] != 0;
			}
			for (i = 0; i < 16; i++)
				ev[i] = 0;
		}
		gf_draw_begin(gfx_ctx);
		if (last >= 0) {
			file_ok = save(idx);
			screen(idx, last);
			if (idx == NKEYS)
				done_ms = ms;
		}
		i = (RUN_MS - ms) * (gfx_w - 1) / RUN_MS;	/* remaining time */
		gfx_rect(0, gfx_h - 6, i, gfx_h - 1, 0x00c000);
		gfx_rect(i + 1, gfx_h - 6, gfx_w - 1, gfx_h - 1, 0x000000);
		gf_draw_end(gfx_ctx);
		if (done_ms >= 0 && ms - done_ms > 5000)
			break;
		usleep(POLL_MS * 1000);
	}
out:
	gf_dev_detach(gfx_dev);
	return 0;
}
