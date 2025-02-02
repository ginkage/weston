/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <libweston/libweston.h>
#include <libweston/xwayland-api.h>

#include "weston-desktop-shell-server-protocol.h"

enum animation_type {
	ANIMATION_NONE,

	ANIMATION_ZOOM,
	ANIMATION_FADE,
	ANIMATION_DIM_LAYER,
};

enum fade_type {
	FADE_IN,
	FADE_OUT
};

struct focus_surface {
	struct weston_curtain *curtain;
};

struct workspace {
	struct weston_layer layer;

	struct wl_list focus_list;
	struct wl_listener seat_destroyed_listener;

	struct focus_surface *fsurf_front;
	struct focus_surface *fsurf_back;
	struct weston_view_animation *focus_animation;
};

struct shell_output {
	struct desktop_shell  *shell;
	struct weston_output  *output;
	struct wl_listener    destroy_listener;
	struct wl_list        link;

	struct weston_surface *panel_surface;
	struct wl_listener panel_surface_listener;

	struct weston_surface *background_surface;
	struct wl_listener background_surface_listener;

	struct {
		struct weston_curtain *curtain;
		struct weston_view_animation *animation;
		enum fade_type type;
		struct wl_event_source *startup_timer;
	} fade;
};

struct weston_desktop;
struct desktop_shell {
	struct weston_compositor *compositor;
	struct weston_desktop *desktop;
	const struct weston_xwayland_surface_api *xwayland_surface_api;

	struct wl_listener idle_listener;
	struct wl_listener wake_listener;
	struct wl_listener transform_listener;
	struct wl_listener resized_listener;
	struct wl_listener destroy_listener;
	struct wl_listener show_input_panel_listener;
	struct wl_listener hide_input_panel_listener;
	struct wl_listener update_input_panel_listener;

	struct weston_layer fullscreen_layer;
	struct weston_layer panel_layer;
	struct weston_layer background_layer;
	struct weston_layer lock_layer;
	struct weston_layer input_panel_layer;
	struct weston_layer top_layer;
	struct weston_layer bottom_layer;

	struct wl_listener pointer_focus_listener;
	struct weston_surface *grab_surface;

	struct {
		struct wl_client *client;
		struct wl_resource *desktop_shell;
		struct wl_listener client_destroy_listener;

		unsigned deathcount;
		struct timespec deathstamp;
	} child;

	bool locked;
	bool showing_input_panels;
	bool prepare_event_sent;

	struct text_backend *text_backend;

	struct {
		struct weston_surface *surface;
		pixman_box32_t cursor_rectangle;
	} text_input;

	struct weston_surface *lock_surface;
	struct wl_listener lock_surface_listener;

	struct workspace workspace;

	struct {
		struct wl_resource *binding;
		struct wl_list surfaces;
	} input_panel;

	bool allow_zap;
	uint32_t binding_modifier;
	enum animation_type win_animation_type;
	enum animation_type win_close_animation_type;
	enum animation_type startup_animation_type;
	enum animation_type focus_animation_type;

	struct weston_layer minimized_layer;

	struct wl_listener seat_create_listener;
	struct wl_listener output_create_listener;
	struct wl_listener output_move_listener;
	struct wl_list output_list;
	struct wl_list seat_list;

	struct wl_listener kill_focus_listener;

	enum weston_desktop_shell_panel_position panel_position;

	char *client;

	struct timespec startup_time;
};

struct weston_output *
get_default_output(struct weston_compositor *compositor);

struct weston_view *
get_default_view(struct weston_surface *surface);

struct shell_surface *
get_shell_surface(struct weston_surface *surface);

struct workspace *
get_current_workspace(struct desktop_shell *shell);

void
get_output_work_area(struct desktop_shell *shell,
		     struct weston_output *output,
		     pixman_rectangle32_t *area);

void
lower_fullscreen_layer(struct desktop_shell *shell,
		       struct weston_output *lowering_output,
		       struct shell_surface *orig_shsurf);

void
activate(struct desktop_shell *shell, struct weston_view *view,
	 struct weston_seat *seat, uint32_t flags);

int
input_panel_setup(struct desktop_shell *shell);
void
input_panel_destroy(struct desktop_shell *shell);

typedef void (*shell_for_each_layer_func_t)(struct desktop_shell *,
					    struct weston_layer *, void *);

void
shell_for_each_layer(struct desktop_shell *shell,
		     shell_for_each_layer_func_t func,
		     void *data);
