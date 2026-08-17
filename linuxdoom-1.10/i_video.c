// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------

static const char rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#define _GNU_SOURCE

#include <assert.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <stdarg.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>

#include <X11/Xlib.h>

#include "d_main.h"
#include "doomstat.h"
#include "i_system.h"
#include "m_argv.h"
#include "v_video.h"

#include "doomdef.h"

#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>

#include "xdg-shell.h"
#include "xdg-shell.c"

#define POINTER_WARP_COUNTDOWN 1
#define MAX_BUFFERS 3
static const int BYTES_PER_PIXEL = 4;

typedef enum
{
	POINTER_EVENT_ENTER = 1 << 0,
	POINTER_EVENT_LEAVE = 1 << 1,
	POINTER_EVENT_MOTION = 1 << 2,
	POINTER_EVENT_BUTTON = 1 << 3,
	POINTER_EVENT_AXIS = 1 << 4,
	POINTER_EVENT_AXIS_SOURCE = 1 << 5,
	POINTER_EVENT_AXIS_STOP = 1 << 6,
	POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
} PointerEventMask;

typedef struct
{
	uint32_t event_mask;
	wl_fixed_t surface_x, surface_y;
	uint32_t button, state;
	uint32_t time;
	uint32_t serial;
	struct
	{
		bool valid;
		wl_fixed_t value;
		int32_t discrete;
	} axes[2];
	uint32_t axis_source;
} PointerEvent;

typedef struct
{
	struct wl_buffer* wl_buffer;
	void* data;
	boolean busy;
	int width, height, stride;
} WaylandBuffer;

static struct WaylandState
{
	struct wl_display* wl_display;
	struct wl_registry* wl_registry;
	struct wl_compositor* wl_compositor;
	struct xdg_wm_base* xdg_wm_base;
	struct wl_shm* wl_shm;

	struct wl_surface* wl_surface;
	struct xdg_surface* xdg_surface;
	struct xdg_toplevel* xdg_toplevel;

	struct wl_seat* wl_seat;
	struct wl_keyboard* wl_keyboard;
	struct xkb_state* xkb_state;
	struct xkb_context* xkb_context;
	struct xkb_keymap* xkb_keymap;
	struct wl_pointer* wl_pointer;

	PointerEvent pointer_event;

	int width, height;
	int offset;
	boolean closed;
	boolean fullscreen;
	uint32_t configure_serial;

	WaylandBuffer buffers[MAX_BUFFERS];
} state = {};

int X_width;
int X_height;

// Fake mouse handling.
// This cannot work properly w/o DGA.
// Needs an invisible mouse cursor at least.
boolean grabMouse;
int doPointerWarp = POINTER_WARP_COUNTDOWN;

// Blocky mode,
// replace each 320x200 pixel with multiply*multiply pixels.
// According to Dave Taylor, it still is a bonehead thing
// to use ....
static int multiply = 1;

static WaylandBuffer* image; // fixme

static boolean frame_submitted;
static unsigned int color_map[256];

static void handle_wayland_error(struct wl_display* display)
{
	int err = wl_display_get_error(display);

	if (err == EPROTO)
	{
		uint32_t id;
		const struct wl_interface* interface = NULL;
		int code = wl_display_get_protocol_error(display, &interface, &id);
		fprintf(stderr, "Protocol error from interface %p with id %d: %s\n", interface, id,
				strerror(errno));
	}

	I_Error("Error while dispatching wayland events: %d\n", err);
}

static void randname(char* buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;

	for (int i = 0; i < 6; ++i)
	{
		buf[i] = 'A' + (r & 15) + (r & 16) * 2;
		r >>= 5;
	}
}

static int create_shm_file(void)
{
	int retries = 100;

	do
	{
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;

		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0)
		{
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

static int allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;

	int ret;

	do
	{
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
	{
		close(fd);
		return -1;
	}

	return fd;
}

static void wl_buffer_release(void* data, struct wl_buffer* wl_buffer)
{
	// WaylandBuffer* buffer = data;
	// buffer->busy = false;
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static struct wl_buffer* create_buffer()
{
	const int width = state.width, height = state.height;
	const int stride = width * BYTES_PER_PIXEL;
	const int size = stride * height;

	int fd = allocate_shm_file(size);
	if (fd == -1)
	{
		I_Error("creating a buffer file for %d B failed: %s\n", size, strerror(errno));
	}

	unsigned int* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED_VALIDATE, fd, 0);
	if (data == MAP_FAILED)
	{
		close(fd);
		I_Error("mmap failed: %s\n", strerror(errno));
	}

	assert(state.wl_shm != NULL);
	struct wl_shm_pool* pool = wl_shm_create_pool(state.wl_shm, fd, size);
	assert(pool);

	struct wl_buffer* buffer
		= wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
	assert(buffer);

	assert(wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL) != -1);

	for (int y = 0; y < SCREENHEIGHT; y++)
	{
		for (int x = 0; x < SCREENWIDTH; x++)
		{
			const int screen_pixel_index = x + y * SCREENWIDTH;
			const byte palette_index = ((byte*)image->data)[screen_pixel_index];
			const unsigned int palette_color = color_map[palette_index];
			data[screen_pixel_index] = palette_color;
		}
	}

	wl_shm_pool_destroy(pool);
	assert(close(fd) != -1);
	munmap(data, size);

	return buffer;
}

static const struct wl_callback_listener wl_surface_frame_listener;

// todo: should probably make a separate event queue for rendering to avoid unneccessary callbacks
// to this during the event poll at the start of the tick.
// We only want to commit a surface at the end of I_FinishUpdate.
static void redraw(void* data, struct wl_callback* callback, uint32_t time)
{
	if (callback != NULL)
		wl_callback_destroy(callback);

	callback = wl_surface_frame(state.wl_surface);
	assert(callback != NULL);

	assert(wl_callback_add_listener(callback, &wl_surface_frame_listener, NULL) != -1);

	if (state.configure_serial != 0)
	{
		xdg_surface_ack_configure(state.xdg_surface, state.configure_serial);
		state.configure_serial = 0;
	}

	if (frame_submitted == false)
	{
		struct wl_buffer* wl_buffer = create_buffer();
		wl_surface_attach(state.wl_surface, wl_buffer, 0, 0);
		wl_surface_damage_buffer(state.wl_surface, 0, 0, INT32_MAX, INT32_MAX);
		frame_submitted = true;
		printf("we did it\n");
	}
	else
	{
		printf("too soon\n");
	}

	wl_surface_commit(state.wl_surface);
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = redraw,
};

static void xdg_surface_configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial)
{
	state.configure_serial = serial;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void xdg_toplevel_configure(void* data, struct xdg_toplevel* xdg_toplevel, int width,
								   int height, struct wl_array* wl_states)
{
	if (width == 0 || height == 0)
		return;

	state.width = width;
	state.height = height;
}

static void xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel)
{
	state.closed = true;
}

static void xdg_toplevel_configure_bounds(void* data, struct xdg_toplevel* toplevel, int32_t width,
										  int32_t height)
{
}

static void xdg_toplevel_wm_capabilities(void* data, struct xdg_toplevel* toplevel,
										 struct wl_array* capabilities)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
	.configure_bounds = xdg_toplevel_configure_bounds,
	.wm_capabilities = xdg_toplevel_wm_capabilities,
};

static void wl_shm_format(void* data, struct wl_shm* shm, uint32_t format) {}

static int lastmousex = 0;
static int lastmousey = 0;
boolean mousemoved = false;

static const struct wl_shm_listener wl_shm_listener = {
	.format = wl_shm_format,
};

static void wl_pointer_enter(void* data, struct wl_pointer* wl_pointer, uint32_t serial,
							 struct wl_surface* surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	state.pointer_event.event_mask |= POINTER_EVENT_ENTER;
	state.pointer_event.serial = serial;
	state.pointer_event.surface_x = surface_x;
	state.pointer_event.surface_y = surface_y;
}

static void wl_pointer_leave(void* data, struct wl_pointer* wl_pointer, uint32_t serial,
							 struct wl_surface* surface)
{
	state.pointer_event.serial = serial;
	state.pointer_event.event_mask |= POINTER_EVENT_LEAVE;
}

static void wl_pointer_motion(void* data, struct wl_pointer* wl_pointer, uint32_t time,
							  wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	state.pointer_event.event_mask |= POINTER_EVENT_MOTION;
	state.pointer_event.time = time;
	state.pointer_event.surface_x = surface_x;
	state.pointer_event.surface_y = surface_y;
}

static void wl_pointer_button(void* data, struct wl_pointer* wl_pointer, uint32_t serial,
							  uint32_t time, uint32_t button, uint32_t button_state)
{
	state.pointer_event.event_mask |= POINTER_EVENT_BUTTON;
	state.pointer_event.time = time;
	state.pointer_event.serial = serial;
	state.pointer_event.button = button;
	state.pointer_event.state = button_state;
}

static void wl_pointer_axis(void* data, struct wl_pointer* wl_pointer, uint32_t time, uint32_t axis,
							wl_fixed_t value)
{
	state.pointer_event.event_mask |= POINTER_EVENT_AXIS;
	state.pointer_event.time = time;
	state.pointer_event.axes[axis].valid = true;
	state.pointer_event.axes[axis].value = value;
}

static void wl_pointer_axis_source(void* data, struct wl_pointer* wl_pointer, uint32_t axis_source)
{
	state.pointer_event.event_mask |= POINTER_EVENT_AXIS_SOURCE;
	state.pointer_event.axis_source = axis_source;
}

static void wl_pointer_axis_stop(void* data, struct wl_pointer* wl_pointer, uint32_t time,
								 uint32_t axis)
{
	state.pointer_event.time = time;
	state.pointer_event.event_mask |= POINTER_EVENT_AXIS_STOP;
	state.pointer_event.axes[axis].valid = true;
}

static void wl_pointer_axis_discrete(void* data, struct wl_pointer* wl_pointer, uint32_t axis,
									 int32_t discrete)
{
	state.pointer_event.event_mask |= POINTER_EVENT_AXIS_DISCRETE;
	state.pointer_event.axes[axis].valid = true;
	state.pointer_event.axes[axis].discrete = discrete;
}

static void wl_pointer_frame(void* data, struct wl_pointer* wl_pointer)
{
	PointerEvent* event = &state.pointer_event;
	// fprintf(stderr, "pointer frame @ %d: ", event->time);

	if (event->event_mask & POINTER_EVENT_ENTER)
	{
		// fprintf(stderr, "entered %f, %f ", wl_fixed_to_double(event->surface_x),
		// 		wl_fixed_to_double(event->surface_y));
	}

	if (event->event_mask & POINTER_EVENT_LEAVE)
	{
		// fprintf(stderr, "leave");
	}

	event_t e = {
		.type = ev_mouse,
	};

	// todo: probably wanna do relative pointer wayland extension
	if (event->event_mask & POINTER_EVENT_MOTION)
	{
		// fprintf(stderr, "motion %f, %f ", wl_fixed_to_double(event->surface_x),
		// 		wl_fixed_to_double(event->surface_y));

		// fixme: gotta translate all these to wl_pointer

		// event.data1 = (X_event.xmotion.state & Button1Mask)
		// 			| (X_event.xmotion.state & Button2Mask ? 2 : 0)
		// 			| (X_event.xmotion.state & Button3Mask ? 4 : 0);
		// event.data2 = (X_event.xmotion.x - lastmousex) << 2;
		// event.data3 = (lastmousey - X_event.xmotion.y) << 2;
		//
		// if (event.data2 || event.data3)
		// {
		// 	lastmousex = X_event.xmotion.x;
		// 	lastmousey = X_event.xmotion.y;
		// 	if (X_event.xmotion.x != X_width / 2 && X_event.xmotion.y != X_height / 2)
		// 	{
		// 		D_PostEvent(&event);
		// 		// fprintf(stderr, "m");
		// 		mousemoved = false;
		// 	}
		// 	else
		// 	{
		// 		mousemoved = true;
		// 	}
		// }
	}

	if (event->event_mask & POINTER_EVENT_BUTTON)
	{
		char* button_state
			= (event->state == WL_POINTER_BUTTON_STATE_RELEASED) ? "released" : "pressed";
		// fprintf(stderr, "button %d %s ", event->button, button_state);

		// fixme: gotta translate all these to wl_pointer

		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
		{
			// event.data1 = (X_event.xbutton.state & Button1Mask)
			// 			| (X_event.xbutton.state & Button2Mask ? 2 : 0)
			// 			| (X_event.xbutton.state & Button3Mask ? 4 : 0)
			// 			| (X_event.xbutton.button == Button1)
			// 			| (X_event.xbutton.button == Button2 ? 2 : 0)
			// 			| (X_event.xbutton.button == Button3 ? 4 : 0);
		}
		else
		{
			// e.data1 = (X_event.xbutton.state & Button1Mask)
			// 		| (X_event.xbutton.state & Button2Mask ? 2 : 0)
			// 		| (X_event.xbutton.state & Button3Mask ? 4 : 0);
			//
			// e.data1 = event.data1 ^ (X_event.xbutton.button == Button1 ? 1 : 0)
			// 		^ (X_event.xbutton.button == Button2 ? 2 : 0)
			// 		^ (X_event.xbutton.button == Button3 ? 4 : 0);
		}

		D_PostEvent(&e);
	}

	uint32_t axis_events = POINTER_EVENT_AXIS | POINTER_EVENT_AXIS_SOURCE | POINTER_EVENT_AXIS_STOP
						 | POINTER_EVENT_AXIS_DISCRETE;

	char* axis_name[2] = {
		[WL_POINTER_AXIS_VERTICAL_SCROLL] = "vertical",
		[WL_POINTER_AXIS_HORIZONTAL_SCROLL] = "horizontal",
	};

	char* axis_source[4] = {
		[WL_POINTER_AXIS_SOURCE_WHEEL] = "wheel",
		[WL_POINTER_AXIS_SOURCE_FINGER] = "finger",
		[WL_POINTER_AXIS_SOURCE_CONTINUOUS] = "continuous",
		[WL_POINTER_AXIS_SOURCE_WHEEL_TILT] = "wheel tilt",
	};

	if (event->event_mask & axis_events)
	{
		for (size_t i = 0; i < 2; ++i)
		{
			if (!event->axes[i].valid)
			{
				continue;
			}
			// fprintf(stderr, "%s axis ", axis_name[i]);
			if (event->event_mask & POINTER_EVENT_AXIS)
			{
				// fprintf(stderr, "value %f ", wl_fixed_to_double(event->axes[i].value));
			}
			if (event->event_mask & POINTER_EVENT_AXIS_DISCRETE)
			{
				// fprintf(stderr, "discrete %d ", event->axes[i].discrete);
			}
			if (event->event_mask & POINTER_EVENT_AXIS_SOURCE)
			{
				// fprintf(stderr, "via %s ", axis_source[event->axis_source]);
			}
			if (event->event_mask & POINTER_EVENT_AXIS_STOP)
			{
				// fprintf(stderr, "(stopped) ");
			}
		}
	}

	// fprintf(stderr, "\n");
	memset(event, 0, sizeof(*event));
}

static const struct wl_pointer_listener wl_pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

//
//  Translates the key currently in X_event
//

static int xlatekey(xkb_keysym_t sym)
{
	switch (sym)
	{
	case XKB_KEY_Left:
		return KEY_LEFTARROW;
		break;
	case XKB_KEY_Right:
		return KEY_RIGHTARROW;
		break;
	case XKB_KEY_Down:
		return KEY_DOWNARROW;
		break;
	case XKB_KEY_Up:
		return KEY_UPARROW;
		break;
	case XKB_KEY_Escape:
		return KEY_ESCAPE;
		break;
	case XKB_KEY_Return:
		return KEY_ENTER;
		break;
	case XKB_KEY_Tab:
		return KEY_TAB;
		break;
	case XKB_KEY_F1:
		return KEY_F1;
		break;
	case XKB_KEY_F2:
		return KEY_F2;
		break;
	case XKB_KEY_F3:
		return KEY_F3;
		break;
	case XKB_KEY_F4:
		return KEY_F4;
		break;
	case XKB_KEY_F5:
		return KEY_F5;
		break;
	case XKB_KEY_F6:
		return KEY_F6;
		break;
	case XKB_KEY_F7:
		return KEY_F7;
		break;
	case XKB_KEY_F8:
		return KEY_F8;
		break;
	case XKB_KEY_F9:
		return KEY_F9;
		break;
	case XKB_KEY_F10:
		return KEY_F10;
		break;
	case XKB_KEY_F11:
		return KEY_F11;
		break;
	case XKB_KEY_F12:
		return KEY_F12;
		break;

	case XKB_KEY_BackSpace:
	case XKB_KEY_Delete:
		return KEY_BACKSPACE;
		break;

	case XKB_KEY_Pause:
		return KEY_PAUSE;
		break;

	case XKB_KEY_KP_Equal:
	case XKB_KEY_equal:
		return KEY_EQUALS;
		break;

	case XKB_KEY_KP_Subtract:
	case XKB_KEY_minus:
		return KEY_MINUS;
		break;

	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
		return KEY_RSHIFT;
		break;

	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
		return KEY_RCTRL;
		break;

	case XKB_KEY_Alt_L:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Meta_R:
		return KEY_RALT;
		break;

	default: // todo: check this
		if (sym >= XKB_KEY_space && sym <= XKB_KEY_asciitilde)
			return sym - XKB_KEY_space + ' ';
		if (sym >= 'A' && sym <= 'Z')
			return sym - 'A' + 'a';
		break;
	}

	return sym;
}

static void wl_keyboard_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format,
							   int32_t fd, uint32_t size)
{
	assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);

	char* map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED_VALIDATE, fd, 0);
	if (map_shm == MAP_FAILED)
		I_Error("mmap failed: %s\n", strerror(errno));

	assert(state.xkb_context);
	struct xkb_keymap* keymap = xkb_keymap_new_from_string(
		state.xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

	munmap(map_shm, size);
	close(fd);

	struct xkb_state* xkb_state = xkb_state_new(keymap);
	xkb_keymap_unref(state.xkb_keymap);
	xkb_state_unref(state.xkb_state);
	state.xkb_keymap = keymap;
	state.xkb_state = xkb_state;
}

static void wl_keyboard_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial,
							  struct wl_surface* surface, struct wl_array* keys)
{
	uint32_t* key;

	wl_array_for_each(key, keys)
	{
		char buf[128];

		xkb_keysym_t sym = xkb_state_key_get_one_sym(state.xkb_state, *key + 8);
		xkb_keysym_get_name(sym, buf, sizeof(buf));

		xkb_state_key_get_utf8(state.xkb_state, *key + 8, buf, sizeof(buf));
	}
}

static void wl_keyboard_key(void* data, struct wl_keyboard* wl_keyboard, uint32_t serial,
							uint32_t time, uint32_t key, uint32_t key_state)
{
	char buf[128];

	uint32_t keycode = key + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(state.xkb_state, keycode);
	xkb_keysym_get_name(sym, buf, sizeof(buf));

	const char* action = (key_state == WL_KEYBOARD_KEY_STATE_PRESSED) ? "press" : "release";
	// fprintf(stderr, "key %s: sym: %-12s (%d), ", action, buf, sym);

	xkb_state_key_get_utf8(state.xkb_state, keycode, buf, sizeof(buf));
	// fprintf(stderr, "utf8: '%s'\n", buf);

	if (key_state == WL_KEYBOARD_KEY_STATE_REPEATED)
		return;

	event_t event = {
		.type = (key_state == WL_KEYBOARD_KEY_STATE_PRESSED) ? ev_keydown : ev_keyup,
		.data1 = xlatekey(sym),
	};

	D_PostEvent(&event);

	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED && sym == XKB_KEY_f)
	{
		if (state.fullscreen == true)
			xdg_toplevel_unset_fullscreen(state.xdg_toplevel);
		else
			xdg_toplevel_set_fullscreen(state.xdg_toplevel, NULL);

		state.fullscreen = !state.fullscreen;
	}
}

static void wl_keyboard_leave(void* data, struct wl_keyboard* wl_keyboard, uint32_t serial,
							  struct wl_surface* surface)
{
}

static void wl_keyboard_modifiers(void* data, struct wl_keyboard* wl_keyboard, uint32_t serial,
								  uint32_t mods_depressed, uint32_t mods_latched,
								  uint32_t mods_locked, uint32_t group)
{
	xkb_state_update_mask(state.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void wl_keyboard_repeat_info(void* data, struct wl_keyboard* wl_keyboard, int32_t rate,
									int32_t delay)
{
}

static struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

static void wl_seat_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities)
{
	boolean have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
	if (have_pointer && state.wl_pointer == NULL)
	{
		printf("setting wl_pointer\n");
		state.wl_pointer = wl_seat_get_pointer(state.wl_seat);
		wl_pointer_add_listener(state.wl_pointer, &wl_pointer_listener, NULL);
	}
	else if (!have_pointer && state.wl_pointer != NULL)
	{
		printf("releasing wl_pointer\n");
		wl_pointer_release(state.wl_pointer);
		state.wl_pointer = NULL;
	}

	bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

	if (have_keyboard && state.wl_keyboard == NULL)
	{
		state.wl_keyboard = wl_seat_get_keyboard(state.wl_seat);
		wl_keyboard_add_listener(state.wl_keyboard, &wl_keyboard_listener, NULL);
	}
	else if (!have_keyboard && state.wl_keyboard != NULL)
	{
		wl_keyboard_release(state.wl_keyboard);
		state.wl_keyboard = NULL;
	}
}

static void wl_seat_name(void* data, struct wl_seat* wl_seat, const char* name)
{
	fprintf(stderr, "seat name: %s\n", name);
}

const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities,
	.name = wl_seat_name,
};

// todo: check for minimum supported versions
static void registry_global(void* data, struct wl_registry* wl_registry, uint32_t name,
							const char* interface, uint32_t version)
{
	if (strcmp(interface, wl_shm_interface.name) == 0)
	{
		state.wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 2);
		assert(state.wl_shm);

		assert(wl_shm_add_listener(state.wl_shm, &wl_shm_listener, NULL) != -1);
	}
	else if (strcmp(interface, wl_compositor_interface.name) == 0)
	{
		state.wl_compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 6);

		assert(state.wl_compositor);
	}
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
	{
		state.xdg_wm_base = wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 6);
		assert(state.xdg_wm_base);

		assert(xdg_wm_base_add_listener(state.xdg_wm_base, &xdg_wm_base_listener, NULL) != -1);
	}
	else if (strcmp(interface, wl_seat_interface.name) == 0)
	{
		state.wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 10);

		wl_seat_add_listener(state.wl_seat, &wl_seat_listener, NULL);
	}
}

static void registry_global_remove(void* data, struct wl_registry* wl_registry, unsigned int name)
{
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

void I_ShutdownGraphics(void) {}

//
// I_StartFrame
//
void I_StartFrame(void)
{
	// er?
}

//
// I_StartTic
//
void I_StartTic(void)
{
	int num_dispatched = 0;
	do
	{
		const struct timespec timeout = { .tv_sec = 0, .tv_nsec = 1e6 };
		num_dispatched = wl_display_dispatch_timeout(state.wl_display, &timeout);

		if (num_dispatched == -1)
		{
			handle_wayland_error(state.wl_display);
		}
	} while (num_dispatched > 0);
}

//
// I_UpdateNoBlit
//
void I_UpdateNoBlit(void)
{
	// what is this?
}

//
// I_FinishUpdate
//
void I_FinishUpdate(void)
{
	static int lasttic;
	int tics;
	int i;
	// UNUSED static unsigned char *bigscreen=0;

	// draws little dots on the bottom of the screen
	if (devparm)
	{
		i = I_GetTime();
		tics = i - lasttic;
		lasttic = i;
		if (tics > 20)
			tics = 20;

		for (i = 0; i < tics * 2; i += 2)
			screens[0][(SCREENHEIGHT - 1) * SCREENWIDTH + i] = 0xff;
		for (; i < 20 * 2; i += 2)
			screens[0][(SCREENHEIGHT - 1) * SCREENWIDTH + i] = 0x0;
	}

	// scales the screen size before blitting it
	if (multiply == 2)
	{
		unsigned int* olineptrs[2];
		unsigned int* ilineptr;
		int x, y;
		unsigned int twoopixels;
		unsigned int twomoreopixels;
		unsigned int fouripixels;

		ilineptr = (unsigned int*)(screens[0]);
		for (i = 0; i < 2; i++)
			olineptrs[i] = (unsigned int*)&image->data[i * X_width];

		y = SCREENHEIGHT;
		while (y--)
		{
			x = SCREENWIDTH;
			do
			{
				fouripixels = *ilineptr++;
				twoopixels = (fouripixels & 0xff000000) | ((fouripixels >> 8) & 0xffff00)
						   | ((fouripixels >> 16) & 0xff);
				twomoreopixels = ((fouripixels << 16) & 0xff000000)
							   | ((fouripixels << 8) & 0xffff00) | (fouripixels & 0xff);
#ifdef __BIG_ENDIAN__
				*olineptrs[0]++ = twoopixels;
				*olineptrs[1]++ = twoopixels;
				*olineptrs[0]++ = twomoreopixels;
				*olineptrs[1]++ = twomoreopixels;
#else
				*olineptrs[0]++ = twomoreopixels;
				*olineptrs[1]++ = twomoreopixels;
				*olineptrs[0]++ = twoopixels;
				*olineptrs[1]++ = twoopixels;
#endif
			} while (x -= 4);
			olineptrs[0] += X_width / 4;
			olineptrs[1] += X_width / 4;
		}
	}
	else if (multiply == 3)
	{
		unsigned int* olineptrs[3];
		unsigned int* ilineptr;
		int x, y;
		unsigned int fouropixels[3];
		unsigned int fouripixels;

		ilineptr = (unsigned int*)(screens[0]);
		for (i = 0; i < 3; i++)
			olineptrs[i] = (unsigned int*)&image->data[i * X_width];

		y = SCREENHEIGHT;
		while (y--)
		{
			x = SCREENWIDTH;
			do
			{
				fouripixels = *ilineptr++;
				fouropixels[0] = (fouripixels & 0xff000000) | ((fouripixels >> 8) & 0xff0000)
							   | ((fouripixels >> 16) & 0xffff);
				fouropixels[1] = ((fouripixels << 8) & 0xff000000) | (fouripixels & 0xffff00)
							   | ((fouripixels >> 8) & 0xff);
				fouropixels[2] = ((fouripixels << 16) & 0xffff0000) | ((fouripixels << 8) & 0xff00)
							   | (fouripixels & 0xff);
#ifdef __BIG_ENDIAN__
				*olineptrs[0]++ = fouropixels[0];
				*olineptrs[1]++ = fouropixels[0];
				*olineptrs[2]++ = fouropixels[0];
				*olineptrs[0]++ = fouropixels[1];
				*olineptrs[1]++ = fouropixels[1];
				*olineptrs[2]++ = fouropixels[1];
				*olineptrs[0]++ = fouropixels[2];
				*olineptrs[1]++ = fouropixels[2];
				*olineptrs[2]++ = fouropixels[2];
#else
				*olineptrs[0]++ = fouropixels[2];
				*olineptrs[1]++ = fouropixels[2];
				*olineptrs[2]++ = fouropixels[2];
				*olineptrs[0]++ = fouropixels[1];
				*olineptrs[1]++ = fouropixels[1];
				*olineptrs[2]++ = fouropixels[1];
				*olineptrs[0]++ = fouropixels[0];
				*olineptrs[1]++ = fouropixels[0];
				*olineptrs[2]++ = fouropixels[0];
#endif
			} while (x -= 4);
			olineptrs[0] += 2 * X_width / 4;
			olineptrs[1] += 2 * X_width / 4;
			olineptrs[2] += 2 * X_width / 4;
		}
	}
	else if (multiply == 4)
	{
		// Broken. Gotta fix this some day.
		void Expand4(unsigned*, double*);
		Expand4((unsigned*)(screens[0]), (double*)(image->data));
	}

	memcpy(image->data, screens[0], SCREENWIDTH * SCREENHEIGHT);
	frame_submitted = false;
	printf("ok I'm ready\n");
	do
	{
		if (wl_display_dispatch_timeout(state.wl_display, NULL) == -1)
			handle_wayland_error(state.wl_display);
	} while (frame_submitted == false);
}

//
// I_ReadScreen
//
void I_ReadScreen(byte* scr)
{
	memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
void I_SetPalette(byte* palette)
{
	int i;

	// set the X colormap entries
	for (i = 0; i < 256; i++)
	{
		byte r, b, g;
		r = gammatable[usegamma][*palette++];
		g = gammatable[usegamma][*palette++];
		b = gammatable[usegamma][*palette++];

		color_map[i] = (r << 16) | (g << 8) | b;
	}
}

void I_InitGraphics(void)
{
	char* displayname;
	char* d;
	int n;
	int pnum;
	int x = 0;
	int y = 0;

	// warning: char format, different type arg
	char xsign = ' ';
	char ysign = ' ';

	int oktodraw;
	unsigned long attribmask;
	int valuemask;
	static boolean firsttime = true;

	if (!firsttime)
		return;
	firsttime = false;

	signal(SIGINT, (void (*)(int))I_Quit);

	if (M_CheckParm("-2"))
		multiply = 2;

	if (M_CheckParm("-3"))
		multiply = 3;

	if (M_CheckParm("-4"))
		multiply = 4;

	X_width = SCREENWIDTH * multiply;
	X_height = SCREENHEIGHT * multiply;

	// check for command-line display name
	if ((pnum = M_CheckParm("-disp"))) // suggest parentheses around assignment
		displayname = myargv[pnum + 1];
	else
		displayname = 0;

	// check if the user wants to grab the mouse (quite unnice)
	grabMouse = !!M_CheckParm("-grabmouse");

	// check for command-line geometry
	if ((pnum = M_CheckParm("-geom"))) // suggest parentheses around assignment
	{
		// warning: char format, different type arg 3,5
		n = sscanf(myargv[pnum + 1], "%c%d%c%d", &xsign, &x, &ysign, &y);

		if (n == 2)
			x = y = 0;
		else if (n == 6)
		{
			if (xsign == '-')
				x = -x;
			if (ysign == '-')
				y = -y;
		}
		else
			I_Error("bad -geom parameter");
	}

	if (setenv("WAYLAND_DEBUG", "client", 1) == -1)
		fprintf(stderr, "failed to set wl debug env: %s", strerror(errno));

	image = malloc(sizeof *image);
	image->data = calloc(SCREENWIDTH * SCREENHEIGHT, sizeof(byte));

	state.wl_display = wl_display_connect(NULL);
	if (state.wl_display == NULL)
		I_Error("Could not open wl_display");

	fprintf(stderr, "connected to wl_display\n");

	state.wl_registry = wl_display_get_registry(state.wl_display), state.width = X_width,
	state.height = X_height, state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS),

	assert(state.wl_registry);
	assert(state.xkb_context);

	assert(wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state) != -1);
	assert(wl_display_roundtrip(state.wl_display) != -1);

	state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
	assert(state.wl_surface);

	state.xdg_surface = xdg_wm_base_get_xdg_surface(state.xdg_wm_base, state.wl_surface);
	assert(state.xdg_surface);
	assert(xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state) != -1);

	state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
	assert(state.xdg_toplevel);
	assert(xdg_toplevel_add_listener(state.xdg_toplevel, &xdg_toplevel_listener, &state) != -1);

	wl_surface_commit(state.wl_surface);
	assert(wl_display_roundtrip(state.wl_display) != -1);
	redraw(&state, NULL, 0);

	// grabs the pointer so it is restricted to this window
	if (grabMouse)
	{
		// todo
	}

	// note: I think this gets allocated in V_Init actually?
	// Was this an unneccessary allocation when multiply was greater than one?
	// Otherwise it was set to the Xlib image data thing.
	// screens[0] = (byte*)malloc(SCREENWIDTH * SCREENHEIGHT);
}

unsigned exptable[256];

void InitExpand(void)
{
	int i;

	for (i = 0; i < 256; i++)
		exptable[i] = i | (i << 8) | (i << 16) | (i << 24);
}

double exptable2[256 * 256];

void InitExpand2(void)
{
	int i;
	int j;
	// UNUSED unsigned	iexp, jexp;
	double* exp;
	union
	{
		double d;
		unsigned u[2];
	} pixel;

	printf("building exptable2...\n");
	exp = exptable2;
	for (i = 0; i < 256; i++)
	{
		pixel.u[0] = i | (i << 8) | (i << 16) | (i << 24);
		for (j = 0; j < 256; j++)
		{
			pixel.u[1] = j | (j << 8) | (j << 16) | (j << 24);
			*exp++ = pixel.d;
		}
	}
	printf("done.\n");
}

int inited;

void Expand4(unsigned* lineptr, double* xline)
{
	double dpixel;
	unsigned x;
	unsigned y;
	unsigned fourpixels;
	unsigned step;
	double* exp;

	exp = exptable2;
	if (!inited)
	{
		inited = 1;
		InitExpand2();
	}

	step = 3 * SCREENWIDTH / 2;

	y = SCREENHEIGHT - 1;
	do
	{
		x = SCREENWIDTH;

		do
		{
			fourpixels = lineptr[0];

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff0000) >> 13));
			xline[0] = dpixel;
			xline[160] = dpixel;
			xline[320] = dpixel;
			xline[480] = dpixel;

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff) << 3));
			xline[1] = dpixel;
			xline[161] = dpixel;
			xline[321] = dpixel;
			xline[481] = dpixel;

			fourpixels = lineptr[1];

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff0000) >> 13));
			xline[2] = dpixel;
			xline[162] = dpixel;
			xline[322] = dpixel;
			xline[482] = dpixel;

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff) << 3));
			xline[3] = dpixel;
			xline[163] = dpixel;
			xline[323] = dpixel;
			xline[483] = dpixel;

			fourpixels = lineptr[2];

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff0000) >> 13));
			xline[4] = dpixel;
			xline[164] = dpixel;
			xline[324] = dpixel;
			xline[484] = dpixel;

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff) << 3));
			xline[5] = dpixel;
			xline[165] = dpixel;
			xline[325] = dpixel;
			xline[485] = dpixel;

			fourpixels = lineptr[3];

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff0000) >> 13));
			xline[6] = dpixel;
			xline[166] = dpixel;
			xline[326] = dpixel;
			xline[486] = dpixel;

			dpixel = *(double*)((int)exp + ((fourpixels & 0xffff) << 3));
			xline[7] = dpixel;
			xline[167] = dpixel;
			xline[327] = dpixel;
			xline[487] = dpixel;

			lineptr += 4;
			xline += 8;
		} while (x -= 16);
		xline += step;
	} while (y--);
}
