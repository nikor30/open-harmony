/* Harmony 900: first native drawing. Fills the screen with colour bars
 * through the QNX Graphics Framework (libgf), holds them 4 s and exits;
 * flash.sh starts the Flash UI afterwards. alarm() ends the program after
 * 8 s whatever happens. Built with tools/qnx/qnxcc.py, see README.md. */
#include <qnx63.h>

static const unsigned colors[] = {
	0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
	0xff00ff, 0xff0000, 0x0000ff, 0x000000,
};

int main(int argc, char **argv, char **envp)
{
	int info[64];
	gf_dev_t dev;
	gf_display_t display;
	gf_layer_t layer;
	gf_surface_t surface;
	gf_context_t ctx;
	int w, h, i, n = sizeof(colors) / sizeof(colors[0]);

	(void)argc; (void)argv; (void)envp;
	alarm(8);
	if (gf_dev_attach(&dev, NULL, NULL) != 0)
		return 1;
	for (i = 0; i < 64; i++)
		info[i] = 0;
	if (gf_display_attach(&display, dev, 0, info) != 0)
		return 2;
	w = ((unsigned short *)info)[GF_DISPINFO_XRES / 2];
	h = ((unsigned short *)info)[GF_DISPINFO_YRES / 2];
	if (w <= 0 || w > 1024 || h <= 0 || h > 1024)
		return 3;
	if (gf_layer_attach(&layer, display, info[GF_DISPINFO_MAIN_LAYER / 4], 0) != 0)
		return 4;
	if (gf_surface_create_layer(&surface, &layer, 1, 0, w, h,
			info[GF_DISPINFO_FORMAT / 4], NULL, 0) != 0)
		return 5;
	if (gf_context_create(&ctx) != 0 || gf_context_set_surface(ctx, surface) != 0)
		return 6;
	gf_layer_set_surfaces(layer, &surface, 1);
	gf_layer_set_src_viewport(layer, 0, 0, w - 1, h - 1);
	gf_layer_set_dst_viewport(layer, 0, 0, w - 1, h - 1);
	gf_layer_enable(layer);
	gf_layer_update(layer, 0);

	gf_draw_begin(ctx);
	for (i = 0; i < n; i++) {
		gf_context_set_fgcolor(ctx, colors[i]);
		gf_draw_rect(ctx, 0, i * h / n, w - 1, (i + 1) * h / n - 1);
	}
	gf_draw_end(ctx);

	sleep(4);
	gf_dev_detach(dev);
	return 0;
}
