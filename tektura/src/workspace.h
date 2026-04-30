/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * workspace.h - Wirtualne pulpity (przestrzenie robocze).
 *
 * Koncepcja:
 *   - Kompozytor zarządza listą workspace'ów (domyślnie 4).
 *   - Każde okno (tektura_toplevel) należy do dokładnie jednego workspace'u.
 *   - Przełączanie workspace'u = pokazanie/ukrycie odpowiednich węzłów sceny.
 *   - Okna na tle (pinned) są widoczne na WSZYSTKICH workspace'ach.
 *   - Z IPC można subskrybować IPC_EVENT_WORKSPACE_CHANGED.
 */

#ifndef TEKTURA_WORKSPACE_H
#define TEKTURA_WORKSPACE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tektura_server;
struct tektura_toplevel;

/* Maksymalna liczba workspace'ów */
#define WORKSPACE_MAX 10

/* ------------------------------------------------------------------ */
/* Struktura workspace'u                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t id;            /* 0-based indeks                          */
	char     name[32];      /* nazwa wyświetlana (np. "1", "Dev", ...) */
	struct wl_list toplevels; /* lista tektura_toplevel.ws_link          */
	bool     visible;       /* czy aktualnie wyświetlany na ekranie    */
} tektura_workspace;

/* ------------------------------------------------------------------ */
/* Menedżer workspace'ów (opaque)                                      */
/* ------------------------------------------------------------------ */

typedef struct tektura_workspace_manager tektura_workspace_manager;

/* ------------------------------------------------------------------ */
/* API publiczne                                                        */
/* ------------------------------------------------------------------ */

/*
 * Inicjalizuje menedżer workspace'ów.
 * count: liczba workspace'ów do utworzenia (1..WORKSPACE_MAX).
 * Domyślnie aktywny jest workspace 0.
 */
tektura_workspace_manager *workspace_manager_init(struct tektura_server *server,
	uint32_t count);

/* Zwraca aktualnie aktywny workspace */
tektura_workspace *workspace_get_active(tektura_workspace_manager *mgr);

/* Zwraca workspace o danym indeksie (NULL jeśli poza zakresem) */
tektura_workspace *workspace_get(tektura_workspace_manager *mgr, uint32_t id);

/* Przełącza na workspace o podanym indeksie */
void workspace_switch(tektura_workspace_manager *mgr, uint32_t id);

/* Przełącza na następny workspace (z zawijaniem) */
void workspace_switch_next(tektura_workspace_manager *mgr);

/* Przełącza na poprzedni workspace (z zawijaniem) */
void workspace_switch_prev(tektura_workspace_manager *mgr);

/* Zwraca indeks aktywnego workspace'u */
uint32_t workspace_get_active_id(tektura_workspace_manager *mgr);

/* Zwraca łączną liczbę workspace'ów */
uint32_t workspace_get_count(tektura_workspace_manager *mgr);

/*
 * Przypisuje okno do workspace'u.
 * Jeśli okno było na innym workspace'u, jest stamtąd usuwane.
 */
void workspace_assign_toplevel(tektura_workspace_manager *mgr,
	struct tektura_toplevel *toplevel, uint32_t ws_id);

/*
 * Przenosi okno na następny workspace.
 */
void workspace_move_toplevel_next(tektura_workspace_manager *mgr,
	struct tektura_toplevel *toplevel);

/*
 * Zwraca indeks workspace'u do którego należy okno (-1 jeśli brak).
 */
int workspace_of_toplevel(tektura_workspace_manager *mgr,
	struct tektura_toplevel *toplevel);

/* Ustawia/usuwa "pinned" — okno widoczne na wszystkich workspace'ach */
void workspace_set_pinned(tektura_workspace_manager *mgr,
	struct tektura_toplevel *toplevel, bool pinned);

/*
 * Niszczy menedżer workspace'ów.
 */
void workspace_manager_destroy(tektura_workspace_manager *mgr);

#endif /* TEKTURA_WORKSPACE_H */
