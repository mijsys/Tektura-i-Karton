/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * output.h - Obsługa monitorów i renderowania klatek.
 */

#ifndef TEKTURA_OUTPUT_H
#define TEKTURA_OUTPUT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

struct tektura_server;

struct tektura_output {
	struct wl_list link;
	struct tektura_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

void server_new_output(struct wl_listener *listener, void *data);

#endif /* TEKTURA_OUTPUT_H */
