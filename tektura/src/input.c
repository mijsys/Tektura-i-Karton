/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * input.c - Obsługa klawiatury, myszki, skrótów klawiszowych i kursora.
 */

#include "input.h"
#include "server.h"
#include "view.h"
#include "idle.h"
#include "workspace.h"
#include "lock.h"
#include "decoration.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct tektura_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

/* Wykonaj akcję ze skrótu klawiszowego */
static void execute_action(struct tektura_server *server, const char *action) {
	if (strcmp(action, "quit") == 0) {
		wl_display_terminate(server->wl_display);
	} else if (strcmp(action, "close_window") == 0) {
		struct tektura_toplevel *top;
		top = wl_container_of(server->toplevels.next, top, link);
		if (&top->link != &server->toplevels)
			wlr_xdg_toplevel_send_close(top->xdg_toplevel);
	} else if (strcmp(action, "lock_screen") == 0) {
		if (server->lock_manager)
			lock_request(server->lock_manager);
	} else if (strcmp(action, "workspace_next") == 0) {
		if (server->workspace_manager)
			workspace_switch_next(server->workspace_manager);
	} else if (strcmp(action, "workspace_prev") == 0) {
		if (server->workspace_manager)
			workspace_switch_prev(server->workspace_manager);
	} else if (strncmp(action, "workspace ", 10) == 0) {
		uint32_t id = (uint32_t)atoi(action + 10);
		if (server->workspace_manager && id > 0)
			workspace_switch(server->workspace_manager, id - 1);
	} else if (strncmp(action, "spawn ", 6) == 0) {
		const char *cmd = action + 6;
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
			_exit(1);
		}
	} else {
		wlr_log(WLR_DEBUG, "input: nieznana akcja '%s'", action);
	}
}

static bool handle_keybinding(struct tektura_server *server,
		uint32_t modifiers, xkb_keysym_t sym) {
	if (!server->config) return false;

	for (int i = 0; i < server->config->keybinding_count; i++) {
		const config_keybinding *kb = &server->config->keybindings[i];
		if (kb->modifiers == modifiers && kb->keysym == sym) {
			execute_action(server, kb->action);
			return true;
		}
	}
	return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct tektura_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct tektura_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
		keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (handle_keybinding(server, modifiers, syms[i]))
				handled = true;
		}
	}

	idle_notify_activity(server->idle_manager);
	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct tektura_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct tektura_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct tektura_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names rules = {0};
	if (server->config) {
		rules.layout  = server->config->input.keyboard_layout;
		rules.variant = server->config->input.keyboard_variant;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context,
		server->config ? &rules : NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	int repeat_rate  = server->config ? server->config->input.repeat_rate  : 25;
	int repeat_delay = server->config ? server->config->input.repeat_delay : 600;
	wlr_keyboard_set_repeat_info(wlr_keyboard, repeat_rate, repeat_delay);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct tektura_server *server,
		struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	idle_notify_activity(server->idle_manager);
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_button(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct tektura_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		if (toplevel) {
			focus_toplevel(toplevel);
			/* Sprawdź czy kliknięcie trafiło w belkę / przyciski SSB */
			tektura_deco_hit hit =
				decoration_hit_test(toplevel->decoration, sx, sy);
			switch (hit) {
			case DECO_HIT_TITLEBAR:
				begin_interactive(toplevel, TEKTURA_CURSOR_MOVE, 0);
				break;
			case DECO_HIT_BTN_CLOSE:
				wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
				break;
			case DECO_HIT_BTN_MINIMIZE:
				/* Minimalizacja — ukryj węzeł sceny */
				wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
				break;
			case DECO_HIT_BTN_MAXIMIZE:
				wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel,
					!toplevel->xdg_toplevel->current.maximized);
				wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
				break;
			case DECO_HIT_NONE:
				break;
			}
		}
	}
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat,
		event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}
