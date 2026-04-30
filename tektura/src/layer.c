/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * layer.c - Implementacja wlr-layer-shell-unstable-v1.
 *
 * Każda surface ma:
 *   - warstwę (background/bottom/top/overlay)
 *   - anchor (do której krawędzi jest przypięta)
 *   - exclusive zone (ile pikseli rezerwuje dla siebie)
 *   - margins
 *
 * Przeliczamy „usable area" każdego wyjścia po uwzględnieniu
 * exclusive zones, a następnie ustawiamy pozycję każdej surface
 * we wlr_scene.
 */

#include "layer.h"
#include "server.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* Struktury wewnętrzne                                                 */
/* ------------------------------------------------------------------ */

struct tektura_layer_surface {
	struct wl_list link;                  /* server->layer_surfaces */
	struct tektura_server *server;

	struct wlr_layer_surface_v1 *wlr_layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

/* ------------------------------------------------------------------ */
/* Układ (arrange)                                                      */
/* ------------------------------------------------------------------ */

void layer_shell_arrange(struct tektura_server *server,
		struct wlr_output *output) {
	int ow, oh;
	wlr_output_effective_resolution(output, &ow, &oh);

	struct wlr_box usable = {.x = 0, .y = 0, .width = ow, .height = oh};

	/* Przetwarzaj warstwy od najniższej do najwyższej */
	enum zwlr_layer_shell_v1_layer layers[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	};

	for (size_t li = 0; li < sizeof(layers)/sizeof(layers[0]); li++) {
		struct tektura_layer_surface *ls;
		wl_list_for_each(ls, &server->layer_surfaces, link) {
			struct wlr_layer_surface_v1 *wls = ls->wlr_layer_surface;
			if (!wls->initialized || wls->output != output) continue;
			if ((enum zwlr_layer_shell_v1_layer)wls->current.layer != layers[li]) continue;

			/* Przekaż usable area do wlroots — wlr_scene_layer_surface_v1
			 * sam przelicza pozycję i wysyła configure */
			wlr_scene_layer_surface_v1_configure(ls->scene_layer, &usable, &usable);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Callbacki                                                            */
/* ------------------------------------------------------------------ */

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_layer_surface *ls = wl_container_of(listener, ls, map);
	layer_shell_arrange(ls->server, ls->wlr_layer_surface->output);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_layer_surface *ls = wl_container_of(listener, ls, unmap);
	if (ls->wlr_layer_surface->output)
		layer_shell_arrange(ls->server, ls->wlr_layer_surface->output);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *wls = ls->wlr_layer_surface;

	if (!wls->initialized) return;
	if (wls->current.committed == 0) return;
	if (wls->output)
		layer_shell_arrange(ls->server, wls->output);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_layer_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	free(ls);
}

/* Popup wewnątrz layer surface */
static void layer_popup_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void layer_popup_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct tektura_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit)
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void layer_surface_new_popup(struct wl_listener *listener, void *data) {
	struct tektura_layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *popup_wlr = data;

	struct tektura_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) return;
	popup->xdg_popup = popup_wlr;

	struct wlr_scene_tree *parent_tree =
		wlr_scene_xdg_surface_create(&ls->scene_layer->tree, popup_wlr->base);
	popup_wlr->base->data = parent_tree;

	popup->commit.notify = layer_popup_commit;
	wl_signal_add(&popup_wlr->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = layer_popup_destroy;
	wl_signal_add(&popup_wlr->events.destroy, &popup->destroy);
}

/* ------------------------------------------------------------------ */
/* Nowa surface layer-shell                                            */
/* ------------------------------------------------------------------ */

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct tektura_server *server = wl_container_of(
		listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *wls = data;

	/* Jeśli klient nie wskazał wyjścia — użyj pierwszego dostępnego */
	if (!wls->output) {
		struct wlr_output_layout_output *lo =
			wl_container_of(server->output_layout->outputs.next, lo, link);
		if (&lo->link == &server->output_layout->outputs) {
			wlr_log(WLR_ERROR, "layer_shell: brak wyjścia — odrzucam surface");
			wlr_layer_surface_v1_destroy(wls);
			return;
		}
		wls->output = lo->output;
	}

	/* Wybierz odpowiednią scenę na podstawie warstwy */
	struct wlr_scene_tree *scene_tree;
	switch (wls->pending.layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		scene_tree = server->layer_tree_background; break;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		scene_tree = server->layer_tree_bottom; break;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		scene_tree = server->layer_tree_top; break;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		scene_tree = server->layer_tree_overlay; break;
	default:
		scene_tree = server->layer_tree_top; break;
	}

	struct tektura_layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) {
		wlr_layer_surface_v1_destroy(wls);
		return;
	}
	ls->server = server;
	ls->wlr_layer_surface = wls;
	ls->scene_layer = wlr_scene_layer_surface_v1_create(scene_tree, wls);
	if (!ls->scene_layer) {
		free(ls);
		wlr_layer_surface_v1_destroy(wls);
		return;
	}

	wls->data = ls->scene_layer;

	ls->map.notify = layer_surface_map;
	wl_signal_add(&wls->surface->events.map, &ls->map);
	ls->unmap.notify = layer_surface_unmap;
	wl_signal_add(&wls->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&wls->surface->events.commit, &ls->commit);
	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&wls->events.destroy, &ls->destroy);
	ls->new_popup.notify = layer_surface_new_popup;
	wl_signal_add(&wls->events.new_popup, &ls->new_popup);

	wl_list_insert(&server->layer_surfaces, &ls->link);

	wlr_log(WLR_DEBUG, "layer_shell: nowa surface warstwa=%d", wls->pending.layer);
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void layer_shell_init(struct tektura_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	wl_list_init(&server->layer_surfaces);

	/* Drzewa sceny dla każdej z warstw — kolejność: bg < bottom < windows < top < overlay */
	server->layer_tree_background = wlr_scene_tree_create(&server->scene->tree);
	server->layer_tree_bottom     = wlr_scene_tree_create(&server->scene->tree);
	/* Okna aplikacji są pomiędzy bottom a top — tworzone w protocols.c */
	server->layer_tree_top        = wlr_scene_tree_create(&server->scene->tree);
	server->layer_tree_overlay    = wlr_scene_tree_create(&server->scene->tree);

	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);

	wlr_log(WLR_INFO, "layer_shell: zainicjowano (wlr-layer-shell-unstable-v1)");
}
