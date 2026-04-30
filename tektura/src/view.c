/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * view.c - Logika okien: fokus, przesuwanie, resize, obsługa zdarzeń xdg-toplevel.
 */

#include "view.h"
#include "server.h"
#include "decoration.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>

static bool csv_contains_appid(const char *csv, const char *app_id) {
	if (!csv || !csv[0] || !app_id || !app_id[0]) return false;
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s", csv);
	for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
		while (*tok && isspace((unsigned char)*tok)) tok++;
		char *end = tok + strlen(tok);
		while (end > tok && isspace((unsigned char)*(end - 1))) {
			*(--end) = '\0';
		}
		if (*tok && strcmp(tok, app_id) == 0) {
			return true;
		}
	}
	return false;
}

static bool client_prefers_csd(const struct tektura_toplevel *toplevel) {
	const char *app_id = toplevel->xdg_toplevel->app_id;
	if (!toplevel->server || !toplevel->server->config) return false;

	config_decorations *dc = &toplevel->server->config->decorations;

	if (csv_contains_appid(dc->force_ssd_apps, app_id)) return false;
	if (csv_contains_appid(dc->force_csd_apps, app_id)) return true;

	if (strcmp(dc->default_mode, "csd") == 0) return true;
	if (strcmp(dc->default_mode, "ssd") == 0) return false;

	/* auto: domyślnie SSB */
	return false;
}

void focus_toplevel(struct tektura_toplevel *toplevel) {
	if (toplevel == NULL) {
		return;
	}
	struct tektura_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}

	/* Zaktualizuj dekoracje: aktywne dla nowego, nieaktywne dla poprzedniego */
	if (toplevel->decoration)
		decoration_update(toplevel->decoration, true);

	/* Dezaktywuj dekoracje poprzedniego okna */
	if (prev_surface) {
		struct tektura_toplevel *prev_top;
		wl_list_for_each(prev_top, &server->toplevels, link) {
			if (prev_top->xdg_toplevel->base->surface == prev_surface) {
				if (prev_top->decoration)
					decoration_update(prev_top->decoration, false);
				break;
			}
		}
	}
}

void reset_cursor_mode(struct tektura_server *server) {
	server->cursor_mode = TEKTURA_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

struct tektura_toplevel *desktop_toplevel_at(struct tektura_server *server,
		double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}
	*surface = scene_surface->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree->node.data;
}

static void process_cursor_move(struct tektura_server *server) {
	struct tektura_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct tektura_server *server) {
	struct tektura_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

void process_cursor_motion(struct tektura_server *server, uint32_t time) {
	if (server->cursor_mode == TEKTURA_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == TEKTURA_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct tektura_toplevel *toplevel = desktop_toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

void begin_interactive(struct tektura_toplevel *toplevel,
		enum tektura_cursor_mode mode, uint32_t edges) {
	struct tektura_server *server = toplevel->server;
	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == TEKTURA_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;
		server->grab_geobox = *geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;
		server->resize_edges = edges;
	}
}

/* --- Obsługa zdarzeń xdg-toplevel --- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	toplevel->pinned = false;
	/* Twórz SSB tylko dla klientów bez własnych CSD */
	if (!client_prefers_csd(toplevel)) {
		toplevel->decoration = decoration_create(toplevel->server, toplevel);
	} else {
		toplevel->decoration = NULL;
	}
	if (toplevel->server->workspace_manager) {
		workspace_assign_toplevel(toplevel->server->workspace_manager,
			toplevel,
			workspace_get_active_id(toplevel->server->workspace_manager));
	}
	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}
	if (toplevel->server->workspace_manager &&
		workspace_of_toplevel(toplevel->server->workspace_manager, toplevel) >= 0) {
		wl_list_remove(&toplevel->ws_link);
	}
	wl_list_remove(&toplevel->link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
	/* Przelicz dekorację po każdej zmianie rozmiaru/tytułu */
	if (toplevel->decoration) {
		bool focused = (toplevel->server->seat->keyboard_state.focused_surface
			== toplevel->xdg_toplevel->base->surface);
		decoration_update(toplevel->decoration, focused);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	decoration_destroy(toplevel->decoration);
	toplevel->decoration = NULL;
	if (toplevel->server->workspace_manager &&
		workspace_of_toplevel(toplevel->server->workspace_manager, toplevel) >= 0) {
		wl_list_remove(&toplevel->ws_link);
	}
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	free(toplevel);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, TEKTURA_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, TEKTURA_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct tektura_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct tektura_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}
