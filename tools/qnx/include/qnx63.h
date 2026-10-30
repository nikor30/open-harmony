/* Minimal declarations for QNX Neutrino 6.3 libc on the Harmony 900.
 * Only what our programs use; link names must be passed to qnxcc.py. */
#ifndef QNX63_H
#define QNX63_H

#define NULL ((void *)0)

int usleep(unsigned int usec);
unsigned int sleep(unsigned int sec);
unsigned int alarm(unsigned int sec);
void exit(int status) __attribute__((noreturn));

/* libcdf_client.so (env_mgr client), signatures from the tolua wrappers
 * in libenv_svc.so: int env_x(args..., int *result). */
int env_service_connect(const char *host);
int env_set_backlight_master(int on, int *result);
int env_set_backlight_master_level(int level, int *result);

/* libgf.so.1 (QNX Graphics Framework), call sequence and argument order as
 * used by flash-gf. Handles are opaque pointers; info structs are only
 * read at the offsets below, so callers pass large enough buffers. */
typedef void *gf_dev_t, *gf_display_t, *gf_layer_t, *gf_surface_t, *gf_context_t;

#define GF_DISPINFO_MAIN_LAYER	4	/* int */
#define GF_DISPINFO_XRES	8	/* unsigned short */
#define GF_DISPINFO_YRES	10	/* unsigned short */
#define GF_DISPINFO_FORMAT	12	/* int */

int gf_dev_attach(gf_dev_t *dev, const char *name, void *info);
void gf_dev_detach(gf_dev_t dev);
int gf_display_attach(gf_display_t *display, gf_dev_t dev, int index, void *info);
int gf_layer_attach(gf_layer_t *layer, gf_display_t display, int index, unsigned flags);
int gf_surface_create_layer(gf_surface_t *surface, gf_layer_t *layers, int nlayers,
		unsigned surface_index, int w, int h, int format, void *palette, unsigned flags);
int gf_layer_set_surfaces(gf_layer_t layer, gf_surface_t *surfaces, int n);
void gf_layer_set_src_viewport(gf_layer_t layer, int x1, int y1, int x2, int y2);
void gf_layer_set_dst_viewport(gf_layer_t layer, int x1, int y1, int x2, int y2);
void gf_layer_enable(gf_layer_t layer);
int gf_layer_update(gf_layer_t layer, unsigned flags);
int gf_context_create(gf_context_t *ctx);
int gf_context_set_surface(gf_context_t ctx, gf_surface_t surface);
void gf_context_set_fgcolor(gf_context_t ctx, unsigned rgb);	/* 0xRRGGBB */
int gf_draw_begin(gf_context_t ctx);
void gf_draw_rect(gf_context_t ctx, int x1, int y1, int x2, int y2);
void gf_draw_end(gf_context_t ctx);

#endif
