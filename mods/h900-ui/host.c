/* Render ohui's screens on the PC for design checks (no QNX, no remote).
 *
 *   gcc -O1 -w -ffreestanding -fno-builtin -Itools/qnx/include -o /tmp/ohui-host mods/h900-ui/host.c
 *   /tmp/ohui-host out.raw <running -1..3> <pressed ticket -1..3> <px> <screen 0 home, 4 settings>
 *
 * px is how far the home screen's pages are turned (208 a page), or the scroll offset of another screen.
 *
 * out.raw is 240x320 RGB565, little endian. */
static unsigned short hostfb[320 * 240];
#define DR_HOST_FB hostfb
#define OHUI_HOST
#include "ohui.c"

static int num(const char *s) { return *s == '-' ? -atoi_s(s + 1) : atoi_s(s); }

int main(int argc, char **argv)
{
	int fd;

	if (argc != 6)
		return 2;
	gfx_w = 240;
	gfx_h = 320;
	dr_px = hostfb;
	dr_stride = 240;
	now = 100000;
	scopy(clock_text, "20:15", sizeof(clock_text));
	battery = 6;
	curtain_init();
	running = num(argv[2]);
	if (num(argv[3]) >= 0) {
		press_kind = T_ACT;
		press_index = num(argv[3]);
		gesture = G_TAP;
		sent_t = now - 100;
	}
	stack[0].kind = num(argv[5]);
	if (num(argv[5]) == S_HOME)
		pager.x = pager.target = num(argv[4]) << 8;
	else
		stack[0].sy = num(argv[4]) << 8;
	stack[0].shown = now - 5000;
	render();
	fd = creat(argv[1], 0644);
	write(fd, hostfb, sizeof(hostfb));
	close(fd);
	return 0;
}
