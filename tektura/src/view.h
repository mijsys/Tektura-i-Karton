/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * view.h - Logika okien (toplevels): pozycja, fokus, przesuwanie, resize.
 */

#ifndef TEKTURA_VIEW_H
#define TEKTURA_VIEW_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_surface.h>
#include "decoration.h"

struct tektura_server;

struct tektura_toplevel {
	struct wl_list link;
	struct wl_list ws_link;      /* link do listy okien w workspace */
	struct tektura_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	uint32_t workspace_id;       /* indeks przypisanego workspace  */
	bool pinned;                 /* widoczne na wszystkich WS       */
	struct tektura_decoration *decoration; /* SSB belka i ramka */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct tektura_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

void focus_toplevel(struct tektura_toplevel *toplevel);
void reset_cursor_mode(struct tektura_server *server);
void begin_interactive(struct tektura_toplevel *toplevel,
	enum tektura_cursor_mode mode, uint32_t edges);

struct tektura_toplevel *desktop_toplevel_at(struct tektura_server *server,
	double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);

void process_cursor_motion(struct tektura_server *server, uint32_t time);

#endif /* TEKTURA_VIEW_H */
