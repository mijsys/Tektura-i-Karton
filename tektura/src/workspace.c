/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * workspace.c - Implementacja wirtualnych pulpitów.
 *
 * Mechanizm pokazywania/ukrywania okien opiera się na
 * wlr_scene_node_set_enabled() — węzły sceny nieaktywnego
 * workspace'u są po prostu wyłączone (nie renderowane).
 */

#include "workspace.h"
#include "server.h"
#include "view.h"
#include "ipc.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_scene.h>

/* ------------------------------------------------------------------ */
/* Struktura wewnętrzna                                                 */
/* ------------------------------------------------------------------ */

struct tektura_workspace_manager {
	struct tektura_server *server;
	uint32_t               count;
	uint32_t               active;
	tektura_workspace      workspaces[WORKSPACE_MAX];
};

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                           */
/* ------------------------------------------------------------------ */

/*
 * Ustawia widoczność wszystkich okien należących do workspace'u.
 * Okna przypięte (toplevel->pinned == true) są zawsze widoczne.
 */
static void ws_apply_visibility(tektura_workspace_manager *mgr,
		tektura_workspace *ws, bool visible) {
	ws->visible = visible;

	struct tektura_toplevel *toplevel;
	wl_list_for_each(toplevel, &ws->toplevels, ws_link) {
		/* Okna przypięte są widoczne niezależnie od aktywnego workspace */
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node,
			visible || toplevel->pinned);
	}
}

static void notify_ipc(tektura_workspace_manager *mgr) {
	if (!mgr->server->ipc) return;
	char payload[32];
	snprintf(payload, sizeof(payload), "%u", mgr->active);
	ipc_broadcast_event(mgr->server->ipc, IPC_EVENT_WORKSPACE_CHANGED, payload);
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

tektura_workspace_manager *workspace_manager_init(struct tektura_server *server,
		uint32_t count) {
	if (count < 1 || count > WORKSPACE_MAX) {
		count = 4;
	}
	tektura_workspace_manager *mgr = calloc(1, sizeof(*mgr));
	if (!mgr) return NULL;

	mgr->server = server;
	mgr->count  = count;
	mgr->active = 0;

	for (uint32_t i = 0; i < count; i++) {
		mgr->workspaces[i].id      = i;
		mgr->workspaces[i].visible = (i == 0);
		wl_list_init(&mgr->workspaces[i].toplevels);
		snprintf(mgr->workspaces[i].name, sizeof(mgr->workspaces[i].name),
			"%u", i + 1);
	}

	wlr_log(WLR_INFO, "workspace: zainicjalizowano %u przestrzeni roboczych", count);
	return mgr;
}

tektura_workspace *workspace_get_active(tektura_workspace_manager *mgr) {
	return &mgr->workspaces[mgr->active];
}

uint32_t workspace_get_active_id(tektura_workspace_manager *mgr) {
	return mgr->active;
}

uint32_t workspace_get_count(tektura_workspace_manager *mgr) {
	return mgr->count;
}

tektura_workspace *workspace_get(tektura_workspace_manager *mgr, uint32_t id) {
	if (id >= mgr->count) return NULL;
	return &mgr->workspaces[id];
}

void workspace_switch(tektura_workspace_manager *mgr, uint32_t id) {
	if (id >= mgr->count || id == mgr->active) return;

	/* Ukryj aktualny */
	ws_apply_visibility(mgr, &mgr->workspaces[mgr->active], false);
	/* Pokaż nowy */
	mgr->active = id;
	ws_apply_visibility(mgr, &mgr->workspaces[mgr->active], true);

	wlr_log(WLR_DEBUG, "workspace: przełączono na %u (%s)",
		id, mgr->workspaces[id].name);
	notify_ipc(mgr);
}

void workspace_switch_next(tektura_workspace_manager *mgr) {
	workspace_switch(mgr, (mgr->active + 1) % mgr->count);
}

void workspace_switch_prev(tektura_workspace_manager *mgr) {
	workspace_switch(mgr, (mgr->active + mgr->count - 1) % mgr->count);
}

void workspace_assign_toplevel(tektura_workspace_manager *mgr,
		struct tektura_toplevel *toplevel, uint32_t ws_id) {
	if (ws_id >= mgr->count) return;

	/* Usuń z poprzedniego workspace'u jeśli jest */
	int prev = workspace_of_toplevel(mgr, toplevel);
	if (prev >= 0) {
		wl_list_remove(&toplevel->ws_link);
	}

	/* Dodaj do nowego */
	wl_list_insert(&mgr->workspaces[ws_id].toplevels, &toplevel->ws_link);
	toplevel->workspace_id = ws_id;

	/* Ustaw widoczność zgodnie z aktywnym workspace'em */
	bool visible = toplevel->pinned || (ws_id == mgr->active);
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, visible);
}

void workspace_move_toplevel_next(tektura_workspace_manager *mgr,
		struct tektura_toplevel *toplevel) {
	int cur = workspace_of_toplevel(mgr, toplevel);
	if (cur < 0) return;
	uint32_t next = ((uint32_t)cur + 1) % mgr->count;
	workspace_assign_toplevel(mgr, toplevel, next);
}

int workspace_of_toplevel(tektura_workspace_manager *mgr,
		struct tektura_toplevel *toplevel) {
	for (uint32_t i = 0; i < mgr->count; i++) {
		struct tektura_toplevel *t;
		wl_list_for_each(t, &mgr->workspaces[i].toplevels, ws_link) {
			if (t == toplevel) return (int)i;
		}
	}
	return -1;
}

void workspace_set_pinned(tektura_workspace_manager *mgr,
		struct tektura_toplevel *toplevel, bool pinned) {
	if (!mgr || !toplevel) return;
	toplevel->pinned = pinned;
	if (pinned) {
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
		return;
	}
	int ws = workspace_of_toplevel(mgr, toplevel);
	if (ws >= 0) {
		bool visible = ((uint32_t)ws == mgr->active);
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, visible);
	}
}

void workspace_manager_destroy(tektura_workspace_manager *mgr) {
	if (!mgr) return;
	free(mgr);
}
