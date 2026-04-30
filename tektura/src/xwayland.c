/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * xwayland.c - Podstawowa integracja Xwayland.
 */

#include "xwayland.h"
#include "server.h"

#include <stdlib.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_scene.h>

#if TEKTURA_HAS_XWAYLAND

#include <wlr/xwayland.h>

struct tektura_xwayland_view {
	struct tektura_server *server;
	struct wlr_xwayland_surface *surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};

static void xsurface_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_xwayland_view *view = wl_container_of(listener, view, map);
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	if (view->server->seat) {
		wlr_seat_keyboard_notify_enter(view->server->seat,
			view->surface->surface, NULL, 0, NULL);
	}
}

static void xsurface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_xwayland_view *view = wl_container_of(listener, view, unmap);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
}

static void xsurface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_xwayland_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	if (view->scene_tree) {
		wlr_scene_node_destroy(&view->scene_tree->node);
	}
	free(view);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *surface = data;

	if (!surface || !surface->surface) {
		return;
	}

	struct tektura_xwayland_view *view = calloc(1, sizeof(*view));
	if (!view) return;
	view->server = server;
	view->surface = surface;

	view->scene_tree = wlr_scene_xwayland_surface_create(&server->scene->tree, surface);
	if (!view->scene_tree) {
		free(view);
		return;
	}

	view->map.notify = xsurface_map;
	wl_signal_add(&surface->events.map, &view->map);
	view->unmap.notify = xsurface_unmap;
	wl_signal_add(&surface->events.unmap, &view->unmap);
	view->destroy.notify = xsurface_destroy;
	wl_signal_add(&surface->events.destroy, &view->destroy);

	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
}

bool xwayland_init(struct tektura_server *server) {
	server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
	if (!server->xwayland) {
		wlr_log(WLR_ERROR, "xwayland: nie udało się uruchomić");
		return false;
	}

	server->new_xwayland_surface.notify = server_new_xwayland_surface;
	wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);

	if (server->xwayland->display_name) {
		setenv("DISPLAY", server->xwayland->display_name, 1);
	}
	wlr_log(WLR_INFO, "xwayland: aktywny DISPLAY=%s",
		server->xwayland->display_name ? server->xwayland->display_name : "(unknown)");
	return true;
}

void xwayland_destroy(struct tektura_server *server) {
	if (!server || !server->xwayland) return;
	wl_list_remove(&server->new_xwayland_surface.link);
	wlr_xwayland_destroy(server->xwayland);
	server->xwayland = NULL;
}

#else

bool xwayland_init(struct tektura_server *server) {
	(void)server;
	wlr_log(WLR_INFO, "xwayland: brak wsparcia w buildzie wlroots (pomijam)");
	return true;
}

void xwayland_destroy(struct tektura_server *server) {
	(void)server;
}

#endif
