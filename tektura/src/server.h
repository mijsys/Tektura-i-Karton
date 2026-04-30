/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * server.h - Serce systemu. Główna struktura kompozytora.
 */

#ifndef TEKTURA_SERVER_H
#define TEKTURA_SERVER_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#if __has_include(<wlr/xwayland.h>)
#include <wlr/xwayland.h>
#define TEKTURA_HAS_XWAYLAND 1
#else
#define TEKTURA_HAS_XWAYLAND 0
struct wlr_xwayland;
#endif
#include "security_manager.h"
#include "ipc.h"
#include "i18n.h"
#include "lock.h"
#include "workspace.h"
#include "config.h"
#include "idle.h"
#include "decoration.h"
#include "wallpaper.h"

enum tektura_cursor_mode {
	TEKTURA_CURSOR_PASSTHROUGH,
	TEKTURA_CURSOR_MOVE,
	TEKTURA_CURSOR_RESIZE,
};

struct tektura_toplevel;

struct tektura_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_compositor *compositor;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum tektura_cursor_mode cursor_mode;
	struct tektura_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	/* layer-shell (panele, docks, tapeta) */
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_list layer_surfaces;
	struct wl_listener new_layer_surface;
	struct wlr_scene_tree *layer_tree_background;
	struct wlr_scene_tree *layer_tree_bottom;
	struct wlr_scene_tree *layer_tree_top;
	struct wlr_scene_tree *layer_tree_overlay;

	/* xdg-decoration (belki tytułowe SSB) */
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wl_listener new_xdg_decoration;

	/* Xwayland (aplikacje X11) */
	struct wlr_xwayland *xwayland;
	struct wl_listener new_xwayland_surface;

	/* wlr-output-management (zdalny control monitorów) */
	struct wlr_output_manager_v1 *output_mgr;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;

	/* System uprawnień aplikacji */
	tektura_security_manager *security_manager;

	/* Bezpieczna komunikacja z resztą środowiska */
	tektura_ipc *ipc;

	/* Blokada ekranu (ext-session-lock-v1) */
	tektura_lock_manager *lock_manager;

	/* Wirtualne pulpity */
	tektura_workspace_manager *workspace_manager;

	/* Konfiguracja (tektura.ini) */
	tektura_config *config;

	/* Zarządzanie bezczynnoścą (idle/dpms/suspend) */
	tektura_idle_manager *idle_manager;

	/* Konfiguracja tapet per-output */
	tektura_wallpaper_manager *wallpaper_manager;
};

void server_init(struct tektura_server *server);
void server_run(struct tektura_server *server, const char *startup_cmd);
void server_destroy(struct tektura_server *server);

#endif /* TEKTURA_SERVER_H */
