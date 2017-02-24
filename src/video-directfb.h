#ifndef __MB_VIDEO_DIRECTFB__
#define __MB_VIDEO_DIRECTFB__

#include <stdint.h>
#include <cairo/cairo.h>

struct mbv_dfb_window;


/**
 * Gets a cairo context for drawing
 * to the window
 */
cairo_t *
mbv_dfb_window_cairo_begin(struct mbv_dfb_window *window);


/**
 * Ends a cairo drawing session and
 * unlocks the surface
 */
void
mbv_dfb_window_cairo_end(struct mbv_dfb_window *window);


void
mbv_dfb_getscreensize(int *width, int *height);


void
mbv_dfb_window_update(struct mbv_dfb_window * const window,
	int update);


int
mbv_dfb_window_blit_buffer(
	struct mbv_dfb_window *window, void *buf, int width, int height,
	const int x, const int y);


struct mbv_dfb_window*
mbv_dfb_window_new(
	struct mbv_window *window,
        int x,
        int y,
        int width,
        int height,
	mbv_repaint_handler repaint_handler);


struct mbv_dfb_window*
mbv_dfb_window_getchildwindow(struct mbv_dfb_window *window,
	int x, int y, int width, int height,
	mbv_repaint_handler repaint_handler);


void
mbv_dfb_window_destroy(struct mbv_dfb_window *win);


struct mbv_dfb_window*
mbv_dfb_getrootwindow(void);


/**
 * mbv_init() -- Initialize video device
 */
struct mbv_dfb_window *
mbv_dfb_init(struct mbv_window *root_window, int argc, char **argv);

void
mbv_dfb_destroy();

#endif
