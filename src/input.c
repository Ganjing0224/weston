/*
 * Copyright © 2013 Intel Corporation
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

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>

#include "../shared/os-compatibility.h"
#include "compositor.h"

static void
empty_region(pixman_region32_t *region)
{
	pixman_region32_fini(region);
	pixman_region32_init(region);
}

static void unbind_resource(struct wl_resource *resource)
{
	wl_list_remove(&resource->link);
	free(resource);
}

void
weston_seat_repick(struct weston_seat *seat)
{
	const struct weston_pointer_grab_interface *interface;
	struct weston_surface *surface, *focus;
	struct weston_pointer *pointer = seat->seat.pointer;

	if (!pointer)
		return;

	surface = weston_compositor_pick_surface(seat->compositor,
						 pointer->x,
						 pointer->y,
						 &pointer->current_x,
						 &pointer->current_y);

	if (&surface->surface != pointer->current) {
		interface = pointer->grab->interface;
		weston_pointer_set_current(pointer, &surface->surface);
		interface->focus(pointer->grab, &surface->surface,
				 pointer->current_x,
				 pointer->current_y);
	}

	focus = (struct weston_surface *) pointer->grab->focus;
	if (focus)
		weston_surface_from_global_fixed(focus,
						 pointer->x,
						 pointer->y,
					         &pointer->grab->x,
					         &pointer->grab->y);
}

static void
weston_compositor_idle_inhibit(struct weston_compositor *compositor)
{
	weston_compositor_wake(compositor);
	compositor->idle_inhibit++;
}

static void
weston_compositor_idle_release(struct weston_compositor *compositor)
{
	compositor->idle_inhibit--;
	weston_compositor_wake(compositor);
}

static void
lose_pointer_focus(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer =
		container_of(listener, struct weston_pointer, focus_listener);

	pointer->focus_resource = NULL;
}

static void
lose_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard =
		container_of(listener, struct weston_keyboard, focus_listener);

	keyboard->focus_resource = NULL;
}

static void
lose_touch_focus(struct wl_listener *listener, void *data)
{
	struct weston_touch *touch =
		container_of(listener, struct weston_touch, focus_listener);

	touch->focus_resource = NULL;
}

static void
default_grab_focus(struct weston_pointer_grab *grab,
		   struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	struct weston_pointer *pointer = grab->pointer;

	if (pointer->button_count > 0)
		return;

	weston_pointer_set_focus(pointer, surface, x, y);
}

static void
default_grab_motion(struct weston_pointer_grab *grab,
		    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct wl_resource *resource;

	resource = grab->pointer->focus_resource;
	if (resource)
		wl_pointer_send_motion(resource, time, x, y);
}

static void
default_grab_button(struct weston_pointer_grab *grab,
		    uint32_t time, uint32_t button, uint32_t state_w)
{
	struct weston_pointer *pointer = grab->pointer;
	struct wl_resource *resource;
	uint32_t serial;
	enum wl_pointer_button_state state = state_w;
	struct wl_display *display;

	resource = pointer->focus_resource;
	if (resource) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);
		wl_pointer_send_button(resource, serial, time, button, state_w);
	}

	if (pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED)
		weston_pointer_set_focus(pointer, pointer->current,
					 pointer->current_x,
					 pointer->current_y);
}

static const struct weston_pointer_grab_interface
				default_pointer_grab_interface = {
	default_grab_focus,
	default_grab_motion,
	default_grab_button
};

static void
default_grab_touch_down(struct weston_touch_grab *grab, uint32_t time,
			int touch_id, wl_fixed_t sx, wl_fixed_t sy)
{
	struct weston_touch *touch = grab->touch;
	struct wl_display *display;
	uint32_t serial;

	if (touch->focus_resource && touch->focus) {
		display = wl_client_get_display(touch->focus_resource->client);
		serial = wl_display_next_serial(display);
		wl_touch_send_down(touch->focus_resource, serial, time,
				   &touch->focus->resource,
				   touch_id, sx, sy);
	}
}

static void
default_grab_touch_up(struct weston_touch_grab *grab,
		      uint32_t time, int touch_id)
{
	struct weston_touch *touch = grab->touch;
	struct wl_display *display;
	uint32_t serial;

	if (touch->focus_resource) {
		display = wl_client_get_display(touch->focus_resource->client);
		serial = wl_display_next_serial(display);
		wl_touch_send_up(touch->focus_resource, serial, time, touch_id);
	}
}

static void
default_grab_touch_motion(struct weston_touch_grab *grab, uint32_t time,
			  int touch_id, wl_fixed_t sx, wl_fixed_t sy)
{
	struct weston_touch *touch = grab->touch;

	if (touch->focus_resource) {
		wl_touch_send_motion(touch->focus_resource, time,
				     touch_id, sx, sy);
	}
}

static const struct weston_touch_grab_interface default_touch_grab_interface = {
	default_grab_touch_down,
	default_grab_touch_up,
	default_grab_touch_motion
};

static void
default_grab_key(struct weston_keyboard_grab *grab,
		 uint32_t time, uint32_t key, uint32_t state)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct wl_resource *resource;
	struct wl_display *display;
	uint32_t serial;

	resource = keyboard->focus_resource;
	if (resource) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);
		wl_keyboard_send_key(resource, serial, time, key, state);
	}
}

static struct wl_resource *
find_resource_for_surface(struct wl_list *list, struct wl_surface *surface)
{
	struct wl_resource *r;

	if (!surface)
		return NULL;

	wl_list_for_each(r, list, link) {
		if (r->client == surface->resource.client)
			return r;
	}

	return NULL;
}

static void
default_grab_modifiers(struct weston_keyboard_grab *grab, uint32_t serial,
		       uint32_t mods_depressed, uint32_t mods_latched,
		       uint32_t mods_locked, uint32_t group)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct weston_pointer *pointer = keyboard->seat->pointer;
	struct wl_resource *resource, *pr;

	resource = keyboard->focus_resource;
	if (!resource)
		return;

	wl_keyboard_send_modifiers(resource, serial, mods_depressed,
				   mods_latched, mods_locked, group);

	if (pointer && pointer->focus && pointer->focus != keyboard->focus) {
		pr = find_resource_for_surface(&keyboard->resource_list,
					       pointer->focus);
		if (pr) {
			wl_keyboard_send_modifiers(pr,
						   serial,
						   keyboard->modifiers.mods_depressed,
						   keyboard->modifiers.mods_latched,
						   keyboard->modifiers.mods_locked,
						   keyboard->modifiers.group);
		}
	}
}

static const struct weston_keyboard_grab_interface
				default_keyboard_grab_interface = {
	default_grab_key,
	default_grab_modifiers,
};

WL_EXPORT void
weston_pointer_init(struct weston_pointer *pointer)
{
	memset(pointer, 0, sizeof *pointer);
	wl_list_init(&pointer->resource_list);
	pointer->focus_listener.notify = lose_pointer_focus;
	pointer->default_grab.interface = &default_pointer_grab_interface;
	pointer->default_grab.pointer = pointer;
	pointer->grab = &pointer->default_grab;
	wl_signal_init(&pointer->focus_signal);

	/* FIXME: Pick better co-ords. */
	pointer->x = wl_fixed_from_int(100);
	pointer->y = wl_fixed_from_int(100);
}

WL_EXPORT void
weston_pointer_release(struct weston_pointer *pointer)
{
	/* XXX: What about pointer->resource_list? */
	if (pointer->focus_resource)
		wl_list_remove(&pointer->focus_listener.link);
}

WL_EXPORT void
weston_keyboard_init(struct weston_keyboard *keyboard)
{
	memset(keyboard, 0, sizeof *keyboard);
	wl_list_init(&keyboard->resource_list);
	wl_array_init(&keyboard->keys);
	keyboard->focus_listener.notify = lose_keyboard_focus;
	keyboard->default_grab.interface = &default_keyboard_grab_interface;
	keyboard->default_grab.keyboard = keyboard;
	keyboard->grab = &keyboard->default_grab;
	wl_signal_init(&keyboard->focus_signal);
}

WL_EXPORT void
weston_keyboard_release(struct weston_keyboard *keyboard)
{
	/* XXX: What about keyboard->resource_list? */
	if (keyboard->focus_resource)
		wl_list_remove(&keyboard->focus_listener.link);
	wl_array_release(&keyboard->keys);
}

WL_EXPORT void
weston_touch_init(struct weston_touch *touch)
{
	memset(touch, 0, sizeof *touch);
	wl_list_init(&touch->resource_list);
	touch->focus_listener.notify = lose_touch_focus;
	touch->default_grab.interface = &default_touch_grab_interface;
	touch->default_grab.touch = touch;
	touch->grab = &touch->default_grab;
	wl_signal_init(&touch->focus_signal);
}

WL_EXPORT void
weston_touch_release(struct weston_touch *touch)
{
	/* XXX: What about touch->resource_list? */
	if (touch->focus_resource)
		wl_list_remove(&touch->focus_listener.link);
}

static void
seat_send_updated_caps(struct wl_seat *seat)
{
	struct wl_resource *r;
	enum wl_seat_capability caps = 0;

	if (seat->pointer)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (seat->keyboard)
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (seat->touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;

	wl_list_for_each(r, &seat->base_resource_list, link)
		wl_seat_send_capabilities(r, caps);
}

WL_EXPORT void
wl_seat_set_pointer(struct wl_seat *seat, struct weston_pointer *pointer)
{
	if (pointer && (seat->pointer || pointer->seat))
		return; /* XXX: error? */
	if (!pointer && !seat->pointer)
		return;

	seat->pointer = pointer;
	if (pointer)
		pointer->seat = seat;

	seat_send_updated_caps(seat);
}

WL_EXPORT void
wl_seat_set_keyboard(struct wl_seat *seat, struct weston_keyboard *keyboard)
{
	if (keyboard && (seat->keyboard || keyboard->seat))
		return; /* XXX: error? */
	if (!keyboard && !seat->keyboard)
		return;

	seat->keyboard = keyboard;
	if (keyboard)
		keyboard->seat = seat;

	seat_send_updated_caps(seat);
}

WL_EXPORT void
wl_seat_set_touch(struct wl_seat *seat, struct weston_touch *touch)
{
	if (touch && (seat->touch || touch->seat))
		return; /* XXX: error? */
	if (!touch && !seat->touch)
		return;

	seat->touch = touch;
	if (touch)
		touch->seat = seat;

	seat_send_updated_caps(seat);
}

WL_EXPORT void
weston_pointer_set_focus(struct weston_pointer *pointer,
			 struct wl_surface *surface,
			 wl_fixed_t sx, wl_fixed_t sy)
{
	struct weston_keyboard *kbd = pointer->seat->keyboard;
	struct wl_resource *resource, *kr;
	struct wl_display *display;
	uint32_t serial;

	resource = pointer->focus_resource;
	if (resource && pointer->focus != surface) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);
		wl_pointer_send_leave(resource, serial,
				      &pointer->focus->resource);
		wl_list_remove(&pointer->focus_listener.link);
	}

	resource = find_resource_for_surface(&pointer->resource_list,
					     surface);
	if (resource &&
	    (pointer->focus != surface ||
	     pointer->focus_resource != resource)) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);
		if (kbd) {
			kr = find_resource_for_surface(&kbd->resource_list,
						       surface);
			if (kr) {
				wl_keyboard_send_modifiers(kr,
							   serial,
							   kbd->modifiers.mods_depressed,
							   kbd->modifiers.mods_latched,
							   kbd->modifiers.mods_locked,
							   kbd->modifiers.group);
			}
		}
		wl_pointer_send_enter(resource, serial, &surface->resource,
				      sx, sy);
		wl_signal_add(&resource->destroy_signal,
			      &pointer->focus_listener);
		pointer->focus_serial = serial;
	}

	pointer->focus_resource = resource;
	pointer->focus = surface;
	pointer->default_grab.focus = surface;
	wl_signal_emit(&pointer->focus_signal, pointer);
}

WL_EXPORT void
weston_keyboard_set_focus(struct weston_keyboard *keyboard,
			  struct wl_surface *surface)
{
	struct wl_resource *resource;
	struct wl_display *display;
	uint32_t serial;

	if (keyboard->focus_resource && keyboard->focus != surface) {
		resource = keyboard->focus_resource;
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);
		wl_keyboard_send_leave(resource, serial,
				       &keyboard->focus->resource);
		wl_list_remove(&keyboard->focus_listener.link);
	}

	resource = find_resource_for_surface(&keyboard->resource_list,
					     surface);
	if (resource &&
	    (keyboard->focus != surface ||
	     keyboard->focus_resource != resource)) {
		display = wl_client_get_display(resource->client);
		serial = wl_display_next_serial(display);
		wl_keyboard_send_modifiers(resource, serial,
					   keyboard->modifiers.mods_depressed,
					   keyboard->modifiers.mods_latched,
					   keyboard->modifiers.mods_locked,
					   keyboard->modifiers.group);
		wl_keyboard_send_enter(resource, serial, &surface->resource,
				       &keyboard->keys);
		wl_signal_add(&resource->destroy_signal,
			      &keyboard->focus_listener);
		keyboard->focus_serial = serial;
	}

	keyboard->focus_resource = resource;
	keyboard->focus = surface;
	wl_signal_emit(&keyboard->focus_signal, keyboard);
}

WL_EXPORT void
weston_keyboard_start_grab(struct weston_keyboard *keyboard,
			   struct weston_keyboard_grab *grab)
{
	keyboard->grab = grab;
	grab->keyboard = keyboard;

	/* XXX focus? */
}

WL_EXPORT void
weston_keyboard_end_grab(struct weston_keyboard *keyboard)
{
	keyboard->grab = &keyboard->default_grab;
}

WL_EXPORT void
weston_pointer_start_grab(struct weston_pointer *pointer,
			  struct weston_pointer_grab *grab)
{
	const struct weston_pointer_grab_interface *interface;

	pointer->grab = grab;
	interface = pointer->grab->interface;
	grab->pointer = pointer;

	if (pointer->current)
		interface->focus(pointer->grab, pointer->current,
				 pointer->current_x, pointer->current_y);
}

WL_EXPORT void
weston_pointer_end_grab(struct weston_pointer *pointer)
{
	const struct weston_pointer_grab_interface *interface;

	pointer->grab = &pointer->default_grab;
	interface = pointer->grab->interface;
	interface->focus(pointer->grab, pointer->current,
			 pointer->current_x, pointer->current_y);
}

static void
current_surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_pointer *pointer =
		container_of(listener, struct weston_pointer, current_listener);

	pointer->current = NULL;
}

WL_EXPORT void
weston_pointer_set_current(struct weston_pointer *pointer,
			   struct wl_surface *surface)
{
	if (pointer->current)
		wl_list_remove(&pointer->current_listener.link);

	pointer->current = surface;

	if (!surface)
		return;
	
	wl_signal_add(&surface->resource.destroy_signal,
		      &pointer->current_listener);
	pointer->current_listener.notify = current_surface_destroy;
}

WL_EXPORT void
weston_touch_start_grab(struct weston_touch *touch, struct weston_touch_grab *grab)
{
	touch->grab = grab;
	grab->touch = touch;
}

WL_EXPORT void
weston_touch_end_grab(struct weston_touch *touch)
{
	touch->grab = &touch->default_grab;
}

static  void
weston_seat_update_drag_surface(struct weston_seat *seat, int dx, int dy);

static void
clip_pointer_motion(struct weston_seat *seat, wl_fixed_t *fx, wl_fixed_t *fy)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_output *output, *prev = NULL;
	int x, y, old_x, old_y, valid = 0;

	x = wl_fixed_to_int(*fx);
	y = wl_fixed_to_int(*fy);
	old_x = wl_fixed_to_int(seat->seat.pointer->x);
	old_y = wl_fixed_to_int(seat->seat.pointer->y);

	wl_list_for_each(output, &ec->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						   x, y, NULL))
			valid = 1;
		if (pixman_region32_contains_point(&output->region,
						   old_x, old_y, NULL))
			prev = output;
	}

	if (!valid) {
		if (x < prev->x)
			*fx = wl_fixed_from_int(prev->x);
		else if (x >= prev->x + prev->width)
			*fx = wl_fixed_from_int(prev->x +
						prev->width - 1);
		if (y < prev->y)
			*fy = wl_fixed_from_int(prev->y);
		else if (y >= prev->y + prev->current->height)
			*fy = wl_fixed_from_int(prev->y +
						prev->height - 1);
	}
}

/* Takes absolute values */
static void
move_pointer(struct weston_seat *seat, wl_fixed_t x, wl_fixed_t y)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_pointer *pointer = seat->seat.pointer;
	struct weston_output *output;
	int32_t ix, iy;

	clip_pointer_motion(seat, &x, &y);

	weston_seat_update_drag_surface(seat, x - pointer->x, y - pointer->y);

	pointer->x = x;
	pointer->y = y;

	ix = wl_fixed_to_int(x);
	iy = wl_fixed_to_int(y);

	wl_list_for_each(output, &ec->output_list, link)
		if (output->zoom.active &&
		    pixman_region32_contains_point(&output->region,
						   ix, iy, NULL))
			weston_output_update_zoom(output, ZOOM_FOCUS_POINTER);

	weston_seat_repick(seat);

	if (seat->sprite) {
		weston_surface_set_position(seat->sprite,
					    ix - seat->hotspot_x,
					    iy - seat->hotspot_y);
		weston_surface_schedule_repaint(seat->sprite);
	}
}

WL_EXPORT void
notify_motion(struct weston_seat *seat,
	      uint32_t time, wl_fixed_t dx, wl_fixed_t dy)
{
	const struct weston_pointer_grab_interface *interface;
	struct weston_compositor *ec = seat->compositor;
	struct weston_pointer *pointer = seat->seat.pointer;

	weston_compositor_wake(ec);

	move_pointer(seat, pointer->x + dx, pointer->y + dy);

	interface = pointer->grab->interface;
	interface->motion(pointer->grab, time,
			  pointer->grab->x, pointer->grab->y);
}

WL_EXPORT void
notify_motion_absolute(struct weston_seat *seat,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	const struct weston_pointer_grab_interface *interface;
	struct weston_compositor *ec = seat->compositor;
	struct weston_pointer *pointer = seat->seat.pointer;

	weston_compositor_wake(ec);

	move_pointer(seat, x, y);

	interface = pointer->grab->interface;
	interface->motion(pointer->grab, time,
			  pointer->grab->x, pointer->grab->y);
}

WL_EXPORT void
weston_surface_activate(struct weston_surface *surface,
			struct weston_seat *seat)
{
	struct weston_compositor *compositor = seat->compositor;

	if (seat->seat.keyboard) {
		weston_keyboard_set_focus(seat->seat.keyboard, &surface->surface);
		wl_data_device_set_keyboard_focus(&seat->seat);
	}

	wl_signal_emit(&compositor->activate_signal, surface);
}

WL_EXPORT void
notify_button(struct weston_seat *seat, uint32_t time, int32_t button,
	      enum wl_pointer_button_state state)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_pointer *pointer = seat->seat.pointer;
	struct weston_surface *focus =
		(struct weston_surface *) pointer->focus;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (compositor->ping_handler && focus)
			compositor->ping_handler(focus, serial);
		weston_compositor_idle_inhibit(compositor);
		if (pointer->button_count == 0) {
			pointer->grab_button = button;
			pointer->grab_time = time;
			pointer->grab_x = pointer->x;
			pointer->grab_y = pointer->y;
		}
		pointer->button_count++;
	} else {
		weston_compositor_idle_release(compositor);
		pointer->button_count--;
	}

	weston_compositor_run_button_binding(compositor, seat, time, button,
					     state);

	pointer->grab->interface->button(pointer->grab, time, button, state);

	if (pointer->button_count == 1)
		pointer->grab_serial =
			wl_display_get_serial(compositor->wl_display);
}

WL_EXPORT void
notify_axis(struct weston_seat *seat, uint32_t time, uint32_t axis,
	    wl_fixed_t value)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_pointer *pointer = seat->seat.pointer;
	struct weston_surface *focus =
		(struct weston_surface *) pointer->focus;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);

	if (compositor->ping_handler && focus)
		compositor->ping_handler(focus, serial);

	weston_compositor_wake(compositor);

	if (!value)
		return;

	if (weston_compositor_run_axis_binding(compositor, seat,
						   time, axis, value))
		return;

	if (pointer->focus_resource)
		wl_pointer_send_axis(pointer->focus_resource, time, axis,
				     value);
}

WL_EXPORT void
notify_modifiers(struct weston_seat *seat, uint32_t serial)
{
	struct weston_keyboard *keyboard = &seat->keyboard;
	struct weston_keyboard_grab *grab = keyboard->grab;
	uint32_t mods_depressed, mods_latched, mods_locked, group;
	uint32_t mods_lookup;
	enum weston_led leds = 0;
	int changed = 0;

	/* Serialize and update our internal state, checking to see if it's
	 * different to the previous state. */
	mods_depressed = xkb_state_serialize_mods(seat->xkb_state.state,
						  XKB_STATE_DEPRESSED);
	mods_latched = xkb_state_serialize_mods(seat->xkb_state.state,
						XKB_STATE_LATCHED);
	mods_locked = xkb_state_serialize_mods(seat->xkb_state.state,
					       XKB_STATE_LOCKED);
	group = xkb_state_serialize_group(seat->xkb_state.state,
					  XKB_STATE_EFFECTIVE);

	if (mods_depressed != seat->seat.keyboard->modifiers.mods_depressed ||
	    mods_latched != seat->seat.keyboard->modifiers.mods_latched ||
	    mods_locked != seat->seat.keyboard->modifiers.mods_locked ||
	    group != seat->seat.keyboard->modifiers.group)
		changed = 1;

	seat->seat.keyboard->modifiers.mods_depressed = mods_depressed;
	seat->seat.keyboard->modifiers.mods_latched = mods_latched;
	seat->seat.keyboard->modifiers.mods_locked = mods_locked;
	seat->seat.keyboard->modifiers.group = group;

	/* And update the modifier_state for bindings. */
	mods_lookup = mods_depressed | mods_latched;
	seat->modifier_state = 0;
	if (mods_lookup & (1 << seat->xkb_info.ctrl_mod))
		seat->modifier_state |= MODIFIER_CTRL;
	if (mods_lookup & (1 << seat->xkb_info.alt_mod))
		seat->modifier_state |= MODIFIER_ALT;
	if (mods_lookup & (1 << seat->xkb_info.super_mod))
		seat->modifier_state |= MODIFIER_SUPER;
	if (mods_lookup & (1 << seat->xkb_info.shift_mod))
		seat->modifier_state |= MODIFIER_SHIFT;

	/* Finally, notify the compositor that LEDs have changed. */
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info.num_led))
		leds |= LED_NUM_LOCK;
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info.caps_led))
		leds |= LED_CAPS_LOCK;
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info.scroll_led))
		leds |= LED_SCROLL_LOCK;
	if (leds != seat->xkb_state.leds && seat->led_update)
		seat->led_update(seat, leds);
	seat->xkb_state.leds = leds;

	if (changed) {
		grab->interface->modifiers(grab,
					   serial,
					   keyboard->modifiers.mods_depressed,
					   keyboard->modifiers.mods_latched,
					   keyboard->modifiers.mods_locked,
					   keyboard->modifiers.group);
	}
}

static void
update_modifier_state(struct weston_seat *seat, uint32_t serial, uint32_t key,
		      enum wl_keyboard_key_state state)
{
	enum xkb_key_direction direction;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		direction = XKB_KEY_DOWN;
	else
		direction = XKB_KEY_UP;

	/* Offset the keycode by 8, as the evdev XKB rules reflect X's
	 * broken keycode system, which starts at 8. */
	xkb_state_update_key(seat->xkb_state.state, key + 8, direction);

	notify_modifiers(seat, serial);
}

WL_EXPORT void
notify_key(struct weston_seat *seat, uint32_t time, uint32_t key,
	   enum wl_keyboard_key_state state,
	   enum weston_key_state_update update_state)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_keyboard *keyboard = &seat->keyboard;
	struct weston_surface *focus =
		(struct weston_surface *) keyboard->focus;
	struct weston_keyboard_grab *grab = keyboard->grab;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);
	uint32_t *k, *end;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (compositor->ping_handler && focus)
			compositor->ping_handler(focus, serial);

		weston_compositor_idle_inhibit(compositor);
		keyboard->grab_key = key;
		keyboard->grab_time = time;
	} else {
		weston_compositor_idle_release(compositor);
	}

	end = keyboard->keys.data + keyboard->keys.size;
	for (k = keyboard->keys.data; k < end; k++) {
		if (*k == key) {
			/* Ignore server-generated repeats. */
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
				return;
			*k = *--end;
		}
	}
	keyboard->keys.size = (void *) end - keyboard->keys.data;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		k = wl_array_add(&keyboard->keys, sizeof *k);
		*k = key;
	}

	if (grab == &keyboard->default_grab ||
	    grab == &keyboard->input_method_grab) {
		weston_compositor_run_key_binding(compositor, seat, time, key,
						  state);
		grab = keyboard->grab;
	}

	grab->interface->key(grab, time, key, state);

	if (update_state == STATE_UPDATE_AUTOMATIC) {
		update_modifier_state(seat,
				      wl_display_get_serial(compositor->wl_display),
				      key,
				      state);
	}
}

WL_EXPORT void
notify_pointer_focus(struct weston_seat *seat, struct weston_output *output,
		     wl_fixed_t x, wl_fixed_t y)
{
	struct weston_compositor *compositor = seat->compositor;

	if (output) {
		move_pointer(seat, x, y);
		compositor->focus = 1;
	} else {
		compositor->focus = 0;
		/* FIXME: We should call weston_pointer_set_focus(seat,
		 * NULL) here, but somehow that breaks re-entry... */
	}
}

static void
destroy_device_saved_kbd_focus(struct wl_listener *listener, void *data)
{
	struct weston_seat *ws;

	ws = container_of(listener, struct weston_seat,
			  saved_kbd_focus_listener);

	ws->saved_kbd_focus = NULL;
}

WL_EXPORT void
notify_keyboard_focus_in(struct weston_seat *seat, struct wl_array *keys,
			 enum weston_key_state_update update_state)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_keyboard *keyboard = seat->seat.keyboard;
	struct wl_surface *surface;
	uint32_t *k, serial;

	serial = wl_display_next_serial(compositor->wl_display);
	wl_array_copy(&keyboard->keys, keys);
	wl_array_for_each(k, &keyboard->keys) {
		weston_compositor_idle_inhibit(compositor);
		if (update_state == STATE_UPDATE_AUTOMATIC)
			update_modifier_state(seat, serial, *k,
					      WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	/* Run key bindings after we've updated the state. */
	wl_array_for_each(k, &keyboard->keys) {
		weston_compositor_run_key_binding(compositor, seat, 0, *k,
						  WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	surface = seat->saved_kbd_focus;

	if (surface) {
		wl_list_remove(&seat->saved_kbd_focus_listener.link);
		weston_keyboard_set_focus(keyboard, surface);
		seat->saved_kbd_focus = NULL;
	}
}

WL_EXPORT void
notify_keyboard_focus_out(struct weston_seat *seat)
{
	struct weston_compositor *compositor = seat->compositor;
	struct weston_keyboard *keyboard = seat->seat.keyboard;
	uint32_t *k, serial;

	serial = wl_display_next_serial(compositor->wl_display);
	wl_array_for_each(k, &keyboard->keys) {
		weston_compositor_idle_release(compositor);
		update_modifier_state(seat, serial, *k,
				      WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	seat->modifier_state = 0;

	if (keyboard->focus) {
		seat->saved_kbd_focus = keyboard->focus;
		seat->saved_kbd_focus_listener.notify =
			destroy_device_saved_kbd_focus;
		wl_signal_add(&keyboard->focus->resource.destroy_signal,
			      &seat->saved_kbd_focus_listener);
	}

	weston_keyboard_set_focus(keyboard, NULL);
	/* FIXME: We really need keyboard grab cancel here to
	 * let the grab shut down properly.  As it is we leak
	 * the grab data. */
	weston_keyboard_end_grab(keyboard);
}

static void
touch_set_focus(struct weston_seat *ws, struct wl_surface *surface)
{
	struct wl_seat *seat = &ws->seat;
	struct wl_resource *resource;

	if (seat->touch->focus == surface)
		return;

	if (seat->touch->focus_resource)
		wl_list_remove(&seat->touch->focus_listener.link);
	seat->touch->focus = NULL;
	seat->touch->focus_resource = NULL;

	if (surface) {
		resource =
			find_resource_for_surface(&seat->touch->resource_list,
						  surface);
		if (!resource) {
			weston_log("couldn't find resource\n");
			return;
		}

		seat->touch->focus = surface;
		seat->touch->focus_resource = resource;
		wl_signal_add(&resource->destroy_signal,
			      &seat->touch->focus_listener);
	}
}

/**
 * notify_touch - emulates button touches and notifies surfaces accordingly.
 *
 * It assumes always the correct cycle sequence until it gets here: touch_down
 * → touch_update → ... → touch_update → touch_end. The driver is responsible
 * for sending along such order.
 *
 */
WL_EXPORT void
notify_touch(struct weston_seat *seat, uint32_t time, int touch_id,
             wl_fixed_t x, wl_fixed_t y, int touch_type)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_touch *touch = seat->seat.touch;
	struct weston_touch_grab *grab = touch->grab;
	struct weston_surface *es;
	wl_fixed_t sx, sy;

	/* Update grab's global coordinates. */
	touch->grab_x = x;
	touch->grab_y = y;

	switch (touch_type) {
	case WL_TOUCH_DOWN:
		weston_compositor_idle_inhibit(ec);

		seat->num_tp++;

		/* the first finger down picks the surface, and all further go
		 * to that surface for the remainder of the touch session i.e.
		 * until all touch points are up again. */
		if (seat->num_tp == 1) {
			es = weston_compositor_pick_surface(ec, x, y, &sx, &sy);
			touch_set_focus(seat, &es->surface);
		} else if (touch->focus) {
			es = (struct weston_surface *) touch->focus;
			weston_surface_from_global_fixed(es, x, y, &sx, &sy);
		} else {
			/* Unexpected condition: We have non-initial touch but
			 * there is no focused surface.
			 */
			weston_log("touch event received with %d points down"
				   "but no surface focused\n", seat->num_tp);
			return;
		}

		grab->interface->down(grab, time, touch_id, sx, sy);
		break;
	case WL_TOUCH_MOTION:
		es = (struct weston_surface *) touch->focus;
		if (!es)
			break;

		weston_surface_from_global_fixed(es, x, y, &sx, &sy);
		grab->interface->motion(grab, time, touch_id, sx, sy);
		break;
	case WL_TOUCH_UP:
		weston_compositor_idle_release(ec);
		seat->num_tp--;

		grab->interface->up(grab, time, touch_id);
		if (seat->num_tp == 0)
			touch_set_focus(seat, NULL);
		break;
	}
}

static void
pointer_handle_sprite_destroy(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = container_of(listener, struct weston_seat,
						sprite_destroy_listener);

	seat->sprite = NULL;
}

static void
pointer_cursor_surface_configure(struct weston_surface *es,
				 int32_t dx, int32_t dy, int32_t width, int32_t height)
{
	struct weston_seat *seat = es->configure_private;
	int x, y;

	if (width == 0)
		return;

	assert(es == seat->sprite);

	seat->hotspot_x -= dx;
	seat->hotspot_y -= dy;

	x = wl_fixed_to_int(seat->seat.pointer->x) - seat->hotspot_x;
	y = wl_fixed_to_int(seat->seat.pointer->y) - seat->hotspot_y;

	weston_surface_configure(seat->sprite, x, y,
				 width, height);

	empty_region(&es->pending.input);

	if (!weston_surface_is_mapped(es)) {
		wl_list_insert(&es->compositor->cursor_layer.surface_list,
			       &es->layer_link);
		weston_surface_update_transform(es);
	}
}

static void
pointer_unmap_sprite(struct weston_seat *seat)
{
	if (weston_surface_is_mapped(seat->sprite))
		weston_surface_unmap(seat->sprite);

	wl_list_remove(&seat->sprite_destroy_listener.link);
	seat->sprite->configure = NULL;
	seat->sprite->configure_private = NULL;
	seat->sprite = NULL;
}

static void
pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
		   uint32_t serial, struct wl_resource *surface_resource,
		   int32_t x, int32_t y)
{
	struct weston_seat *seat = resource->data;
	struct weston_surface *surface = NULL;

	if (surface_resource)
		surface = surface_resource->data;

	if (seat->seat.pointer->focus == NULL)
		return;
	if (seat->seat.pointer->focus->resource.client != client)
		return;
	if (seat->seat.pointer->focus_serial - serial > UINT32_MAX / 2)
		return;

	if (surface && surface != seat->sprite) {
		if (surface->configure) {
			wl_resource_post_error(&surface->surface.resource,
					       WL_DISPLAY_ERROR_INVALID_OBJECT,
					       "surface->configure already "
					       "set");
			return;
		}
	}

	if (seat->sprite)
		pointer_unmap_sprite(seat);

	if (!surface)
		return;

	wl_signal_add(&surface->surface.resource.destroy_signal,
		      &seat->sprite_destroy_listener);

	surface->configure = pointer_cursor_surface_configure;
	surface->configure_private = seat;
	seat->sprite = surface;
	seat->hotspot_x = x;
	seat->hotspot_y = y;

	if (surface->buffer_ref.buffer)
		pointer_cursor_surface_configure(surface, 0, 0, weston_surface_buffer_width(surface),
								weston_surface_buffer_height(surface));
}

static const struct wl_pointer_interface pointer_interface = {
	pointer_set_cursor
};

static void
handle_drag_surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat;

	seat = container_of(listener, struct weston_seat,
			    drag_surface_destroy_listener);

	seat->drag_surface = NULL;
}

static void
seat_get_pointer(struct wl_client *client, struct wl_resource *resource,
		 uint32_t id)
{
	struct weston_seat *seat = resource->data;
	struct wl_resource *cr;

	if (!seat->seat.pointer)
		return;

        cr = wl_client_add_object(client, &wl_pointer_interface,
				  &pointer_interface, id, seat);
	wl_list_insert(&seat->seat.pointer->resource_list, &cr->link);
	cr->destroy = unbind_resource;

	if (seat->seat.pointer->focus &&
	    seat->seat.pointer->focus->resource.client == client) {
		struct weston_surface *surface;
		wl_fixed_t sx, sy;

		surface = (struct weston_surface *) seat->seat.pointer->focus;
		weston_surface_from_global_fixed(surface,
						 seat->seat.pointer->x,
						 seat->seat.pointer->y,
						 &sx,
						 &sy);
		weston_pointer_set_focus(seat->seat.pointer,
					 seat->seat.pointer->focus,
					 sx,
					 sy);
	}
}

static void
seat_get_keyboard(struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
	struct weston_seat *seat = resource->data;
	struct wl_resource *cr;

	if (!seat->seat.keyboard)
		return;

        cr = wl_client_add_object(client, &wl_keyboard_interface, NULL, id,
				  seat);
	wl_list_insert(&seat->seat.keyboard->resource_list, &cr->link);
	cr->destroy = unbind_resource;

	wl_keyboard_send_keymap(cr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
				seat->xkb_info.keymap_fd,
				seat->xkb_info.keymap_size);

	if (seat->seat.keyboard->focus &&
	    seat->seat.keyboard->focus->resource.client == client) {
		weston_keyboard_set_focus(seat->seat.keyboard,
					  seat->seat.keyboard->focus);
		wl_data_device_set_keyboard_focus(&seat->seat);
	}
}

static void
seat_get_touch(struct wl_client *client, struct wl_resource *resource,
	       uint32_t id)
{
	struct weston_seat *seat = resource->data;
	struct wl_resource *cr;

	if (!seat->seat.touch)
		return;

        cr = wl_client_add_object(client, &wl_touch_interface, NULL, id, seat);
	wl_list_insert(&seat->seat.touch->resource_list, &cr->link);
	cr->destroy = unbind_resource;
}

static const struct wl_seat_interface seat_interface = {
	seat_get_pointer,
	seat_get_keyboard,
	seat_get_touch,
};

static void
bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_seat *seat = data;
	struct wl_resource *resource;
	enum wl_seat_capability caps = 0;

	resource = wl_client_add_object(client, &wl_seat_interface,
					&seat_interface, id, data);
	wl_list_insert(&seat->base_resource_list, &resource->link);
	resource->destroy = unbind_resource;

	if (seat->pointer)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (seat->keyboard)
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (seat->touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;

	wl_seat_send_capabilities(resource, caps);
}

static void
device_handle_new_drag_icon(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat;

	seat = container_of(listener, struct weston_seat,
			    new_drag_icon_listener);

	weston_seat_update_drag_surface(seat, 0, 0);
}

int
weston_compositor_xkb_init(struct weston_compositor *ec,
			   struct xkb_rule_names *names)
{
	if (ec->xkb_context == NULL) {
		ec->xkb_context = xkb_context_new(0);
		if (ec->xkb_context == NULL) {
			weston_log("failed to create XKB context\n");
			return -1;
		}
	}

	if (names)
		ec->xkb_names = *names;
	if (!ec->xkb_names.rules)
		ec->xkb_names.rules = strdup("evdev");
	if (!ec->xkb_names.model)
		ec->xkb_names.model = strdup("pc105");
	if (!ec->xkb_names.layout)
		ec->xkb_names.layout = strdup("us");

	return 0;
}

static void xkb_info_destroy(struct weston_xkb_info *xkb_info)
{
	if (xkb_info->keymap)
		xkb_map_unref(xkb_info->keymap);

	if (xkb_info->keymap_area)
		munmap(xkb_info->keymap_area, xkb_info->keymap_size);
	if (xkb_info->keymap_fd >= 0)
		close(xkb_info->keymap_fd);
}

void
weston_compositor_xkb_destroy(struct weston_compositor *ec)
{
	free((char *) ec->xkb_names.rules);
	free((char *) ec->xkb_names.model);
	free((char *) ec->xkb_names.layout);
	free((char *) ec->xkb_names.variant);
	free((char *) ec->xkb_names.options);

	xkb_info_destroy(&ec->xkb_info);
	xkb_context_unref(ec->xkb_context);
}

static int
weston_xkb_info_new_keymap(struct weston_xkb_info *xkb_info)
{
	char *keymap_str;

	xkb_info->shift_mod = xkb_map_mod_get_index(xkb_info->keymap,
						    XKB_MOD_NAME_SHIFT);
	xkb_info->caps_mod = xkb_map_mod_get_index(xkb_info->keymap,
						   XKB_MOD_NAME_CAPS);
	xkb_info->ctrl_mod = xkb_map_mod_get_index(xkb_info->keymap,
						   XKB_MOD_NAME_CTRL);
	xkb_info->alt_mod = xkb_map_mod_get_index(xkb_info->keymap,
						  XKB_MOD_NAME_ALT);
	xkb_info->mod2_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod2");
	xkb_info->mod3_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod3");
	xkb_info->super_mod = xkb_map_mod_get_index(xkb_info->keymap,
						    XKB_MOD_NAME_LOGO);
	xkb_info->mod5_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod5");

	xkb_info->num_led = xkb_map_led_get_index(xkb_info->keymap,
						  XKB_LED_NAME_NUM);
	xkb_info->caps_led = xkb_map_led_get_index(xkb_info->keymap,
						   XKB_LED_NAME_CAPS);
	xkb_info->scroll_led = xkb_map_led_get_index(xkb_info->keymap,
						     XKB_LED_NAME_SCROLL);

	keymap_str = xkb_map_get_as_string(xkb_info->keymap);
	if (keymap_str == NULL) {
		weston_log("failed to get string version of keymap\n");
		return -1;
	}
	xkb_info->keymap_size = strlen(keymap_str) + 1;

	xkb_info->keymap_fd = os_create_anonymous_file(xkb_info->keymap_size);
	if (xkb_info->keymap_fd < 0) {
		weston_log("creating a keymap file for %lu bytes failed: %m\n",
			(unsigned long) xkb_info->keymap_size);
		goto err_keymap_str;
	}

	xkb_info->keymap_area = mmap(NULL, xkb_info->keymap_size,
				     PROT_READ | PROT_WRITE,
				     MAP_SHARED, xkb_info->keymap_fd, 0);
	if (xkb_info->keymap_area == MAP_FAILED) {
		weston_log("failed to mmap() %lu bytes\n",
			(unsigned long) xkb_info->keymap_size);
		goto err_dev_zero;
	}
	strcpy(xkb_info->keymap_area, keymap_str);
	free(keymap_str);

	return 0;

err_dev_zero:
	close(xkb_info->keymap_fd);
	xkb_info->keymap_fd = -1;
err_keymap_str:
	free(keymap_str);
	return -1;
}

static int
weston_compositor_build_global_keymap(struct weston_compositor *ec)
{
	if (ec->xkb_info.keymap != NULL)
		return 0;

	ec->xkb_info.keymap = xkb_map_new_from_names(ec->xkb_context,
						     &ec->xkb_names,
						     0);
	if (ec->xkb_info.keymap == NULL) {
		weston_log("failed to compile global XKB keymap\n");
		weston_log("  tried rules %s, model %s, layout %s, variant %s, "
			"options %s\n",
			ec->xkb_names.rules, ec->xkb_names.model,
			ec->xkb_names.layout, ec->xkb_names.variant,
			ec->xkb_names.options);
		return -1;
	}

	if (weston_xkb_info_new_keymap(&ec->xkb_info) < 0)
		return -1;

	return 0;
}

WL_EXPORT int
weston_seat_init_keyboard(struct weston_seat *seat, struct xkb_keymap *keymap)
{
	if (seat->has_keyboard)
		return 0;

	if (keymap != NULL) {
		seat->xkb_info.keymap = xkb_map_ref(keymap);
		if (weston_xkb_info_new_keymap(&seat->xkb_info) < 0)
			return -1;
	} else {
		if (weston_compositor_build_global_keymap(seat->compositor) < 0)
			return -1;
		seat->xkb_info = seat->compositor->xkb_info;
		seat->xkb_info.keymap = xkb_map_ref(seat->xkb_info.keymap);
	}

	seat->xkb_state.state = xkb_state_new(seat->xkb_info.keymap);
	if (seat->xkb_state.state == NULL) {
		weston_log("failed to initialise XKB state\n");
		return -1;
	}

	seat->xkb_state.leds = 0;

	weston_keyboard_init(&seat->keyboard);
	wl_seat_set_keyboard(&seat->seat, &seat->keyboard);

	seat->has_keyboard = 1;

	return 0;
}

WL_EXPORT void
weston_seat_init_pointer(struct weston_seat *seat)
{
	if (seat->has_pointer)
		return;

	weston_pointer_init(&seat->pointer);
	wl_seat_set_pointer(&seat->seat, &seat->pointer);

	seat->has_pointer = 1;
}

WL_EXPORT void
weston_seat_init_touch(struct weston_seat *seat)
{
	if (seat->has_touch)
		return;

	weston_touch_init(&seat->touch);
	wl_seat_set_touch(&seat->seat, &seat->touch);

	seat->has_touch = 1;
}

WL_EXPORT void
weston_seat_init(struct weston_seat *seat, struct weston_compositor *ec)
{
	memset(seat, 0, sizeof *seat);

	seat->seat.selection_data_source = NULL;
	wl_list_init(&seat->seat.base_resource_list);
	wl_signal_init(&seat->seat.selection_signal);
	wl_list_init(&seat->seat.drag_resource_list);
	wl_signal_init(&seat->seat.drag_icon_signal);

	seat->has_pointer = 0;
	seat->has_keyboard = 0;
	seat->has_touch = 0;

	wl_display_add_global(ec->wl_display, &wl_seat_interface, seat,
			      bind_seat);

	seat->sprite = NULL;
	seat->sprite_destroy_listener.notify = pointer_handle_sprite_destroy;

	seat->compositor = ec;
	seat->hotspot_x = 16;
	seat->hotspot_y = 16;
	seat->modifier_state = 0;
	seat->num_tp = 0;

	seat->drag_surface_destroy_listener.notify =
		handle_drag_surface_destroy;

	wl_list_insert(ec->seat_list.prev, &seat->link);

	seat->new_drag_icon_listener.notify = device_handle_new_drag_icon;
	wl_signal_add(&seat->seat.drag_icon_signal,
		      &seat->new_drag_icon_listener);

	clipboard_create(seat);

	wl_signal_init(&seat->destroy_signal);
	wl_signal_emit(&ec->seat_created_signal, seat);
}

WL_EXPORT void
weston_seat_release(struct weston_seat *seat)
{
	wl_list_remove(&seat->link);
	/* The global object is destroyed at wl_display_destroy() time. */

	if (seat->sprite)
		pointer_unmap_sprite(seat);

	if (seat->xkb_state.state != NULL)
		xkb_state_unref(seat->xkb_state.state);
	xkb_info_destroy(&seat->xkb_info);

	if (seat->seat.pointer)
		weston_pointer_release(seat->seat.pointer);
	if (seat->seat.keyboard)
		weston_keyboard_release(seat->seat.keyboard);
	if (seat->seat.touch)
		weston_touch_release(seat->seat.touch);

	wl_signal_emit(&seat->destroy_signal, seat);
}

static void
drag_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height)
{
	empty_region(&es->pending.input);

	weston_surface_configure(es,
				 es->geometry.x + sx, es->geometry.y + sy,
				 width, height);
}

static int
device_setup_new_drag_surface(struct weston_seat *ws,
			      struct weston_surface *surface)
{
	struct wl_seat *seat = &ws->seat;

	if (surface->configure) {
		wl_resource_post_error(&surface->surface.resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already set");
		return 0;
	}

	ws->drag_surface = surface;

	weston_surface_set_position(ws->drag_surface,
				    wl_fixed_to_double(seat->pointer->x),
				    wl_fixed_to_double(seat->pointer->y));

	surface->configure = drag_surface_configure;

	wl_signal_add(&surface->surface.resource.destroy_signal,
		       &ws->drag_surface_destroy_listener);

	return 1;
}

static void
device_release_drag_surface(struct weston_seat *seat)
{
	if (weston_surface_is_mapped(seat->drag_surface))
		weston_surface_unmap(seat->drag_surface);

	seat->drag_surface->configure = NULL;
	empty_region(&seat->drag_surface->pending.input);
	wl_list_remove(&seat->drag_surface_destroy_listener.link);
	seat->drag_surface = NULL;
}

static void
device_map_drag_surface(struct weston_seat *seat)
{
	struct wl_list *list;

	if (weston_surface_is_mapped(seat->drag_surface) ||
	    !seat->drag_surface->buffer_ref.buffer)
		return;

	if (seat->sprite && weston_surface_is_mapped(seat->sprite))
		list = &seat->sprite->layer_link;
	else
		list = &seat->compositor->cursor_layer.surface_list;

	wl_list_insert(list, &seat->drag_surface->layer_link);
	weston_surface_update_transform(seat->drag_surface);
	empty_region(&seat->drag_surface->input);
}

static  void
weston_seat_update_drag_surface(struct weston_seat *seat,
				int dx, int dy)
{
	int surface_changed = 0;

	if (!seat->drag_surface && !seat->seat.drag_surface)
		return;

	if (seat->drag_surface && seat->seat.drag_surface &&
	    (&seat->drag_surface->surface.resource !=
	     &seat->seat.drag_surface->resource))
		/* between calls to this funcion we got a new drag_surface */
		surface_changed = 1;

	if (!seat->seat.drag_surface || surface_changed) {
		device_release_drag_surface(seat);
		if (!surface_changed)
			return;
	}

	if (!seat->drag_surface || surface_changed) {
		struct weston_surface *surface = (struct weston_surface *)
			seat->seat.drag_surface;
		if (!device_setup_new_drag_surface(seat, surface))
			return;
	}

	/* the client may not have attached a buffer to the drag surface
	 * when we setup it up, so check if map is needed on every update */
	device_map_drag_surface(seat);

	if (!dx && !dy)
		return;

	weston_surface_set_position(seat->drag_surface,
				    seat->drag_surface->geometry.x + wl_fixed_to_double(dx),
				    seat->drag_surface->geometry.y + wl_fixed_to_double(dy));
}

WL_EXPORT void
weston_compositor_update_drag_surfaces(struct weston_compositor *compositor)
{
	struct weston_seat *seat;

	wl_list_for_each(seat, &compositor->seat_list, link)
		weston_seat_update_drag_surface(seat, 0, 0);
}
