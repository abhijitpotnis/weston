/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2012 Raspberry Pi Foundation
 * Copyright © 2013 Philip Withnall
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>

#include <libudev.h>

#include "config.h"

#include "compositor.h"
#include "launcher-util.h"
#include "pixman-renderer.h"
#include "udev-seat.h"

struct fbdev_compositor {
	struct weston_compositor base;
	uint32_t prev_state;

	struct udev *udev;
	struct tty *tty;
};

struct fbdev_screeninfo {
	unsigned int x_resolution; /* pixels, visible area */
	unsigned int y_resolution; /* pixels, visible area */
	unsigned int width_mm; /* visible screen width in mm */
	unsigned int height_mm; /* visible screen height in mm */
	unsigned int bits_per_pixel;

	size_t buffer_length; /* length of frame buffer memory in bytes */
	size_t line_length; /* length of a line in bytes */
	char id[16]; /* screen identifier */

	pixman_format_code_t pixel_format; /* frame buffer pixel format */
	unsigned int refresh_rate; /* Hertz */
};

struct fbdev_output {
	struct fbdev_compositor *compositor;
	struct weston_output base;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;

	/* Frame buffer details. */
	const char *device; /* ownership shared with fbdev_parameters */
	struct fbdev_screeninfo fb_info;
	void *fb; /* length is fb_info.buffer_length */

	/* pixman details. */
	pixman_image_t *hw_surface;
	pixman_image_t *shadow_surface;
	void *shadow_buf;
	uint8_t depth;
};

struct fbdev_seat {
	struct weston_seat base;
	struct wl_list devices_list;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_monitor_source;
	char *seat_id;
};

struct fbdev_parameters {
	int tty;
	char *device;
};

static const char default_seat[] = "seat0";

static inline struct fbdev_output *
to_fbdev_output(struct weston_output *base)
{
	return container_of(base, struct fbdev_output, base);
}

static inline struct fbdev_seat *
to_fbdev_seat(struct weston_seat *base)
{
	return container_of(base, struct fbdev_seat, base);
}

static inline struct fbdev_compositor *
to_fbdev_compositor(struct weston_compositor *base)
{
	return container_of(base, struct fbdev_compositor, base);
}

static void
fbdev_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct fbdev_output *output = to_fbdev_output(base);
	struct weston_compositor *ec = output->base.compositor;
	pixman_box32_t *rects;
	int nrects, i, src_x, src_y, x1, y1, x2, y2, width, height;

	/* Repaint the damaged region onto the back buffer. */
	pixman_renderer_output_set_buffer(base, output->shadow_surface);
	ec->renderer->repaint_output(base, damage);

	/* Transform and composite onto the frame buffer. */
	width = pixman_image_get_width(output->shadow_surface);
	height = pixman_image_get_height(output->shadow_surface);
	rects = pixman_region32_rectangles(damage, &nrects);

	for (i = 0; i < nrects; i++) {
		switch (base->transform) {
		default:
		case WL_OUTPUT_TRANSFORM_NORMAL:
			x1 = rects[i].x1;
			x2 = rects[i].x2;
			y1 = rects[i].y1;
			y2 = rects[i].y2;
			break;
		case WL_OUTPUT_TRANSFORM_180:
			x1 = width - rects[i].x2;
			x2 = width - rects[i].x1;
			y1 = height - rects[i].y2;
			y2 = height - rects[i].y1;
			break;
		case WL_OUTPUT_TRANSFORM_90:
			x1 = height - rects[i].y2;
			x2 = height - rects[i].y1;
			y1 = rects[i].x1;
			y2 = rects[i].x2;
			break;
		case WL_OUTPUT_TRANSFORM_270:
			x1 = rects[i].y1;
			x2 = rects[i].y2;
			y1 = width - rects[i].x2;
			y2 = width - rects[i].x1;
			break;
		}
		src_x = x1;
		src_y = y1;

		pixman_image_composite32(PIXMAN_OP_SRC,
			output->shadow_surface, /* src */
			NULL /* mask */,
			output->hw_surface, /* dest */
			src_x, src_y, /* src_x, src_y */
			0, 0, /* mask_x, mask_y */
			x1, y1, /* dest_x, dest_y */
			x2 - x1, /* width */
			y2 - y1 /* height */);
	}

	/* Update the damage region. */
	pixman_region32_subtract(&ec->primary_plane.damage,
	                         &ec->primary_plane.damage, damage);

	/* Schedule the end of the frame. We do not sync this to the frame
	 * buffer clock because users who want that should be using the DRM
	 * compositor. FBIO_WAITFORVSYNC blocks and FB_ACTIVATE_VBL requires
	 * panning, which is broken in most kernel drivers.
	 *
	 * Finish the frame synchronised to the specified refresh rate. The
	 * refresh rate is given in mHz and the interval in ms. */
	wl_event_source_timer_update(output->finish_frame_timer,
	                             1000000 / output->mode.refresh);
}

static int
finish_frame_handler(void *data)
{
	struct fbdev_output *output = data;
	uint32_t msec;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	weston_output_finish_frame(&output->base, msec);

	return 1;
}

static pixman_format_code_t
calculate_pixman_format(struct fb_var_screeninfo *vinfo,
                        struct fb_fix_screeninfo *finfo)
{
	/* Calculate the pixman format supported by the frame buffer from the
	 * buffer's metadata. Return 0 if no known pixman format is supported
	 * (since this has depth 0 it's guaranteed to not conflict with any
	 * actual pixman format).
	 *
	 * Documentation on the vinfo and finfo structures:
	 *    http://www.mjmwired.net/kernel/Documentation/fb/api.txt
	 *
	 * TODO: Try a bit harder to support other formats, including setting
	 * the preferred format in the hardware. */
	int type;

	weston_log("Calculating pixman format from:\n"
	           STAMP_SPACE " - type: %i (aux: %i)\n"
	           STAMP_SPACE " - visual: %i\n"
	           STAMP_SPACE " - bpp: %i (grayscale: %i)\n"
	           STAMP_SPACE " - red: offset: %i, length: %i, MSB: %i\n"
	           STAMP_SPACE " - green: offset: %i, length: %i, MSB: %i\n"
	           STAMP_SPACE " - blue: offset: %i, length: %i, MSB: %i\n"
	           STAMP_SPACE " - transp: offset: %i, length: %i, MSB: %i\n",
	           finfo->type, finfo->type_aux, finfo->visual,
	           vinfo->bits_per_pixel, vinfo->grayscale,
	           vinfo->red.offset, vinfo->red.length, vinfo->red.msb_right,
	           vinfo->green.offset, vinfo->green.length,
	           vinfo->green.msb_right,
	           vinfo->blue.offset, vinfo->blue.length,
	           vinfo->blue.msb_right,
	           vinfo->transp.offset, vinfo->transp.length,
	           vinfo->transp.msb_right);

	/* We only handle packed formats at the moment. */
	if (finfo->type != FB_TYPE_PACKED_PIXELS)
		return 0;

	/* We only handle true-colour frame buffers at the moment. */
	if (finfo->visual != FB_VISUAL_TRUECOLOR || vinfo->grayscale != 0)
		return 0;

	/* We only support formats with MSBs on the left. */
	if (vinfo->red.msb_right != 0 || vinfo->green.msb_right != 0 ||
	    vinfo->blue.msb_right != 0)
		return 0;

	/* Work out the format type from the offsets. We only support RGBA and
	 * ARGB at the moment. */
	type = PIXMAN_TYPE_OTHER;

	if ((vinfo->transp.offset >= vinfo->red.offset ||
	     vinfo->transp.length == 0) &&
	    vinfo->red.offset >= vinfo->green.offset &&
	    vinfo->green.offset >= vinfo->blue.offset)
		type = PIXMAN_TYPE_ARGB;
	else if (vinfo->red.offset >= vinfo->green.offset &&
	         vinfo->green.offset >= vinfo->blue.offset &&
	         vinfo->blue.offset >= vinfo->transp.offset)
		type = PIXMAN_TYPE_RGBA;

	if (type == PIXMAN_TYPE_OTHER)
		return 0;

	/* Build the format. */
	return PIXMAN_FORMAT(vinfo->bits_per_pixel, type,
	                     vinfo->transp.length,
	                     vinfo->red.length,
	                     vinfo->green.length,
	                     vinfo->blue.length);
}

static int
calculate_refresh_rate(struct fb_var_screeninfo *vinfo)
{
	uint64_t quot;

	/* Calculate monitor refresh rate. Default is 60 Hz. Units are mHz. */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;

	if (quot > 0) {
		uint64_t refresh_rate;

		refresh_rate = 1000000000000000LLU / quot;
		if (refresh_rate > 200000)
			refresh_rate = 200000; /* cap at 200 Hz */

		return refresh_rate;
	}

	return 60 * 1000; /* default to 60 Hz */
}

static int
fbdev_query_screen_info(struct fbdev_output *output, int fd,
                        struct fbdev_screeninfo *info)
{
	struct fb_var_screeninfo varinfo;
	struct fb_fix_screeninfo fixinfo;

	/* Probe the device for screen information. */
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	/* Store the pertinent data. */
	info->x_resolution = varinfo.xres;
	info->y_resolution = varinfo.yres;
	info->width_mm = varinfo.width;
	info->height_mm = varinfo.height;
	info->bits_per_pixel = varinfo.bits_per_pixel;

	info->buffer_length = fixinfo.smem_len;
	info->line_length = fixinfo.line_length;
	strncpy(info->id, fixinfo.id, sizeof(info->id) / sizeof(*info->id));

	info->pixel_format = calculate_pixman_format(&varinfo, &fixinfo);
	info->refresh_rate = calculate_refresh_rate(&varinfo);

	if (info->pixel_format == 0) {
		weston_log("Frame buffer uses an unsupported format.\n");
		return -1;
	}

	return 1;
}

static int
fbdev_set_screen_info(struct fbdev_output *output, int fd,
                      struct fbdev_screeninfo *info)
{
	struct fb_var_screeninfo varinfo;

	/* Grab the current screen information. */
	if (ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	/* Update the information. */
	varinfo.xres = info->x_resolution;
	varinfo.yres = info->y_resolution;
	varinfo.width = info->width_mm;
	varinfo.height = info->height_mm;
	varinfo.bits_per_pixel = info->bits_per_pixel;

	/* Try to set up an ARGB (x8r8g8b8) pixel format. */
	varinfo.grayscale = 0;
	varinfo.transp.offset = 24;
	varinfo.transp.length = 0;
	varinfo.transp.msb_right = 0;
	varinfo.red.offset = 16;
	varinfo.red.length = 8;
	varinfo.red.msb_right = 0;
	varinfo.green.offset = 8;
	varinfo.green.length = 8;
	varinfo.green.msb_right = 0;
	varinfo.blue.offset = 0;
	varinfo.blue.length = 8;
	varinfo.blue.msb_right = 0;

	/* Set the device's screen information. */
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	return 1;
}

static void fbdev_frame_buffer_destroy(struct fbdev_output *output);

/* Returns an FD for the frame buffer device. */
static int
fbdev_frame_buffer_open(struct fbdev_output *output, const char *fb_dev,
                        struct fbdev_screeninfo *screen_info)
{
	int fd = -1;

	weston_log("Opening fbdev frame buffer.\n");

	/* Open the frame buffer device. */
	fd = open(fb_dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		weston_log("Failed to open frame buffer device ‘%s’: %s\n",
		           fb_dev, strerror(errno));
		return -1;
	}

	/* Grab the screen info. */
	if (fbdev_query_screen_info(output, fd, screen_info) < 0) {
		weston_log("Failed to get frame buffer info: %s\n",
		           strerror(errno));

		close(fd);
		return -1;
	}

	return fd;
}

/* Closes the FD on success or failure. */
static int
fbdev_frame_buffer_map(struct fbdev_output *output, int fd)
{
	int retval = -1;

	weston_log("Mapping fbdev frame buffer.\n");

	/* Map the frame buffer. Write-only mode, since we don't want to read
	 * anything back (because it's slow). */
	output->fb = mmap(NULL, output->fb_info.buffer_length,
	                  PROT_WRITE, MAP_SHARED, fd, 0);
	if (output->fb == MAP_FAILED) {
		weston_log("Failed to mmap frame buffer: %s\n",
		           strerror(errno));
		goto out_close;
	}

	/* Create a pixman image to wrap the memory mapped frame buffer. */
	output->hw_surface =
		pixman_image_create_bits(output->fb_info.pixel_format,
		                         output->fb_info.x_resolution,
		                         output->fb_info.y_resolution,
		                         output->fb,
		                         output->fb_info.line_length);
	if (output->hw_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		goto out_unmap;
	}

	/* Success! */
	retval = 0;

out_unmap:
	if (retval != 0 && output->fb != NULL)
		fbdev_frame_buffer_destroy(output);

out_close:
	if (fd >= 0)
		close(fd);

	return retval;
}

static void
fbdev_frame_buffer_destroy(struct fbdev_output *output)
{
	weston_log("Destroying fbdev frame buffer.\n");

	if (munmap(output->fb, output->fb_info.buffer_length) < 0)
		weston_log("Failed to munmap frame buffer: %s\n",
		           strerror(errno));

	output->fb = NULL;
}

static void fbdev_output_destroy(struct weston_output *base);
static void fbdev_output_disable(struct weston_output *base);

static int
fbdev_output_create(struct fbdev_compositor *compositor,
                    const char *device)
{
	struct fbdev_output *output;
	pixman_transform_t transform;
	int fb_fd;
	int shadow_width, shadow_height;
	int width, height;
	unsigned int bytes_per_pixel;
	struct wl_event_loop *loop;

	weston_log("Creating fbdev output.\n");

	output = calloc(1, sizeof *output);
	if (!output)
		return -1;

	output->compositor = compositor;
	output->device = device;

	/* Create the frame buffer. */
	fb_fd = fbdev_frame_buffer_open(output, device, &output->fb_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto out_free;
	}

	if (fbdev_frame_buffer_map(output, fb_fd) < 0) {
		weston_log("Mapping frame buffer failed.\n");
		goto out_free;
	}

	output->base.repaint = fbdev_output_repaint;
	output->base.destroy = fbdev_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	/* only one static mode in list */
	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = output->fb_info.x_resolution;
	output->mode.height = output->fb_info.y_resolution;
	output->mode.refresh = output->fb_info.refresh_rate;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current = &output->mode;
	output->base.origin = &output->mode;
	output->base.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
	output->base.make = "unknown";
	output->base.model = output->fb_info.id;

	weston_output_init(&output->base, &compositor->base,
	                   0, 0, output->fb_info.width_mm,
	                   output->fb_info.height_mm,
	                   WL_OUTPUT_TRANSFORM_NORMAL);

	width = output->fb_info.x_resolution;
	height = output->fb_info.y_resolution;

	pixman_transform_init_identity(&transform);
	switch (output->base.transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
		shadow_width = width;
		shadow_height = height;
		pixman_transform_rotate(&transform,
			NULL, 0, 0);
		pixman_transform_translate(&transform, NULL,
			0, 0);
		break;
	case WL_OUTPUT_TRANSFORM_180:
		shadow_width = width;
		shadow_height = height;
		pixman_transform_rotate(&transform,
			NULL, -pixman_fixed_1, 0);
		pixman_transform_translate(NULL, &transform,
			pixman_int_to_fixed(shadow_width),
			pixman_int_to_fixed(shadow_height));
		break;
	case WL_OUTPUT_TRANSFORM_270:
		shadow_width = height;
		shadow_height = width;
		pixman_transform_rotate(&transform,
			NULL, 0, pixman_fixed_1);
		pixman_transform_translate(&transform,
			NULL,
			pixman_int_to_fixed(shadow_width),
			0);
		break;
	case WL_OUTPUT_TRANSFORM_90:
		shadow_width = height;
		shadow_height = width;
		pixman_transform_rotate(&transform,
			NULL, 0, -pixman_fixed_1);
		pixman_transform_translate(&transform,
			NULL,
			0,
			pixman_int_to_fixed(shadow_height));
		break;
	}

	bytes_per_pixel = output->fb_info.bits_per_pixel / 8;

	output->shadow_buf = malloc(width * height * bytes_per_pixel);
	output->shadow_surface =
		pixman_image_create_bits(output->fb_info.pixel_format,
		                         shadow_width, shadow_height,
		                         output->shadow_buf,
		                         shadow_width * bytes_per_pixel);
	if (output->shadow_buf == NULL || output->shadow_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		goto out_hw_surface;
	}

	/* No need in transform for normal output */
	if (output->base.transform != WL_OUTPUT_TRANSFORM_NORMAL)
		pixman_image_set_transform(output->shadow_surface, &transform);

	if (pixman_renderer_output_create(&output->base) < 0)
		goto out_shadow_surface;

	loop = wl_display_get_event_loop(compositor->base.wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	wl_list_insert(compositor->base.output_list.prev, &output->base.link);

	weston_log("fbdev output %d×%d px\n",
	           output->mode.width, output->mode.height);
	weston_log_continue(STAMP_SPACE "guessing %d Hz and 96 dpi\n",
	                    output->mode.refresh / 1000);

	return 0;

out_shadow_surface:
	pixman_image_unref(output->shadow_surface);
	output->shadow_surface = NULL;
out_hw_surface:
	free(output->shadow_buf);
	pixman_image_unref(output->hw_surface);
	output->hw_surface = NULL;
	weston_output_destroy(&output->base);
	fbdev_frame_buffer_destroy(output);
out_free:
	free(output);

	return -1;
}

static void
fbdev_output_destroy(struct weston_output *base)
{
	struct fbdev_output *output = to_fbdev_output(base);

	weston_log("Destroying fbdev output.\n");

	/* Close the frame buffer. */
	fbdev_output_disable(base);

	if (base->renderer_state != NULL)
		pixman_renderer_output_destroy(base);

	if (output->shadow_surface != NULL) {
		pixman_image_unref(output->shadow_surface);
		output->shadow_surface = NULL;
	}

	if (output->shadow_buf != NULL) {
		free(output->shadow_buf);
		output->shadow_buf = NULL;
	}

	/* Remove the output. */
	wl_list_remove(&output->base.link);
	weston_output_destroy(&output->base);

	free(output);
}

/* strcmp()-style return values. */
static int
compare_screen_info (const struct fbdev_screeninfo *a,
                     const struct fbdev_screeninfo *b)
{
	if (a->x_resolution == b->x_resolution &&
	    a->y_resolution == b->y_resolution &&
	    a->width_mm == b->width_mm &&
	    a->height_mm == b->height_mm &&
	    a->bits_per_pixel == b->bits_per_pixel &&
	    a->pixel_format == b->pixel_format &&
	    a->refresh_rate == b->refresh_rate)
		return 0;

	return 1;
}

static int
fbdev_output_reenable(struct fbdev_compositor *compositor,
                      struct weston_output *base)
{
	struct fbdev_output *output = to_fbdev_output(base);
	struct fbdev_screeninfo new_screen_info;
	int fb_fd;

	weston_log("Re-enabling fbdev output.\n");

	/* Create the frame buffer. */
	fb_fd = fbdev_frame_buffer_open(output, output->device,
	                                &new_screen_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto err;
	}

	/* Check whether the frame buffer details have changed since we were
	 * disabled. */
	if (compare_screen_info (&output->fb_info, &new_screen_info) != 0) {
		/* Perform a mode-set to restore the old mode. */
		if (fbdev_set_screen_info(output, fb_fd,
		                          &output->fb_info) < 0) {
			weston_log("Failed to restore mode settings. "
			           "Attempting to re-open output anyway.\n");
		}

		/* Remove and re-add the output so that resources depending on
		 * the frame buffer X/Y resolution (such as the shadow buffer)
		 * are re-initialised. */
		fbdev_output_destroy(base);
		fbdev_output_create(compositor, output->device);

		return 0;
	}

	/* Map the device if it has the same details as before. */
	if (fbdev_frame_buffer_map(output, fb_fd) < 0) {
		weston_log("Mapping frame buffer failed.\n");
		goto err;
	}

	return 0;

err:
	return -1;
}

/* NOTE: This leaves output->fb_info populated, caching data so that if
 * fbdev_output_reenable() is called again, it can determine whether a mode-set
 * is needed. */
static void
fbdev_output_disable(struct weston_output *base)
{
	struct fbdev_output *output = to_fbdev_output(base);

	weston_log("Disabling fbdev output.\n");

	if (output->hw_surface != NULL) {
		pixman_image_unref(output->hw_surface);
		output->hw_surface = NULL;
	}

	fbdev_frame_buffer_destroy(output);
}

static void
fbdev_compositor_destroy(struct weston_compositor *base)
{
	struct fbdev_compositor *compositor = to_fbdev_compositor(base);
	struct udev_seat *seat, *next;

	/* Destroy all inputs. */
	wl_list_for_each_safe(seat, next, &compositor->base.seat_list, base.link)
		udev_seat_destroy(seat);

	/* Destroy the output. */
	weston_compositor_shutdown(&compositor->base);

	/* Chain up. */
	compositor->base.renderer->destroy(&compositor->base);
	tty_destroy(compositor->tty);

	free(compositor);
}

static void
vt_func(struct weston_compositor *base, int event)
{
	struct fbdev_compositor *compositor = to_fbdev_compositor(base);
	struct udev_seat *seat;
	struct weston_output *output;

	switch (event) {
	case TTY_ENTER_VT:
		weston_log("entering VT\n");
		compositor->base.focus = 1;
		compositor->base.state = compositor->prev_state;

		wl_list_for_each(output, &compositor->base.output_list, link) {
			fbdev_output_reenable(compositor, output);
		}

		weston_compositor_damage_all(&compositor->base);

		wl_list_for_each(seat, &compositor->base.seat_list, base.link)
			udev_seat_enable(seat, compositor->udev);
		break;
	case TTY_LEAVE_VT:
		weston_log("leaving VT\n");
		wl_list_for_each(seat, &compositor->base.seat_list, base.link)
			udev_seat_disable(seat);

		wl_list_for_each(output, &compositor->base.output_list, link) {
			fbdev_output_disable(output);
		}

		compositor->base.focus = 0;
		compositor->prev_state = compositor->base.state;
		weston_compositor_offscreen(&compositor->base);

		/* If we have a repaint scheduled (from the idle handler), make
		 * sure we cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attemps at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output,
				 &compositor->base.output_list, link) {
			output->repaint_needed = 0;
		}

		break;
	};
}

static void
fbdev_restore(struct weston_compositor *base)
{
	struct fbdev_compositor *compositor = to_fbdev_compositor(base);

	tty_reset(compositor->tty);
}

static void
switch_vt_binding(struct wl_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct fbdev_compositor *ec = data;

	tty_activate_vt(ec->tty, key - KEY_F1 + 1);
}

static struct weston_compositor *
fbdev_compositor_create(struct wl_display *display, int *argc, char *argv[],
                        const char *config_file, struct fbdev_parameters *param)
{
	struct fbdev_compositor *compositor;
	const char *seat = default_seat;
	uint32_t key;

	weston_log("initializing fbdev backend\n");

	compositor = calloc(1, sizeof *compositor);
	if (compositor == NULL)
		return NULL;

	if (weston_compositor_init(&compositor->base, display, argc, argv,
	                           config_file) < 0)
		goto out_free;

	compositor->udev = udev_new();
	if (compositor->udev == NULL) {
		weston_log("Failed to initialize udev context.\n");
		goto out_compositor;
	}

	/* Set up the TTY. */
	compositor->tty = tty_create(&compositor->base, vt_func, param->tty);
	if (!compositor->tty) {
		weston_log("Failed to initialize tty.\n");
		goto out_udev;
	}

	compositor->base.destroy = fbdev_compositor_destroy;
	compositor->base.restore = fbdev_restore;

	compositor->base.focus = 1;
	compositor->prev_state = WESTON_COMPOSITOR_ACTIVE;

	for (key = KEY_F1; key < KEY_F9; key++)
		weston_compositor_add_key_binding(&compositor->base, key,
		                                  MODIFIER_CTRL | MODIFIER_ALT,
		                                  switch_vt_binding,
		                                  compositor);

	if (pixman_renderer_init(&compositor->base) < 0)
		goto out_tty;

	if (fbdev_output_create(compositor, param->device) < 0)
		goto out_pixman;

	udev_seat_create(&compositor->base, compositor->udev, seat);

	return &compositor->base;

out_pixman:
	compositor->base.renderer->destroy(&compositor->base);

out_tty:
	tty_destroy(compositor->tty);

out_udev:
	udev_unref(compositor->udev);

out_compositor:
	weston_compositor_shutdown(&compositor->base);

out_free:
	free(compositor);

	return NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	     const char *config_file)
{
	/* TODO: Ideally, available frame buffers should be enumerated using
	 * udev, rather than passing a device node in as a parameter. */
	struct fbdev_parameters param = {
		.tty = 0, /* default to current tty */
		.device = "/dev/fb0", /* default frame buffer */
	};

	const struct weston_option fbdev_options[] = {
		{ WESTON_OPTION_INTEGER, "tty", 0, &param.tty },
		{ WESTON_OPTION_STRING, "device", 0, &param.device },
	};

	parse_options(fbdev_options, ARRAY_LENGTH(fbdev_options), argc, argv);

	return fbdev_compositor_create(display, argc, argv, config_file,
	                               &param);
}
