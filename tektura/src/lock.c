/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * lock.c - Implementacja blokady ekranu.
 *
 * Protokół ext-session-lock-v1:
 *   Kompozytor "ogłasza" blokadę. Klient blokady (np. karton-lock)
 *   tworzy powierzchnię na każdy monitor. Wejście jest przekierowane
 *   wyłącznie do tych powierzchni do czasu odblokowania.
 */

#include "lock.h"
#include "server.h"
#include "ipc.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_scene.h>

/*
 * Uwaga: ext-session-lock wymaga wlroots >= 0.17 i nagłówka:
 *   wlr/types/wlr_session_lock_v1.h
 * Jeśli Twoja wersja wlroots nie ma tego nagłówka, zamień na
 * wlr/types/wlr_ext_session_lock_v1.h w zależności od dystrybucji.
 */
#include <wlr/types/wlr_session_lock_v1.h>

/* ------------------------------------------------------------------ */
/* Struktury wewnętrzne                                                 */
/* ------------------------------------------------------------------ */

/* Jedna Surface blokady per monitor */
typedef struct {
	struct wl_list link;
	struct tektura_lock_manager *mgr;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener map;
	struct wl_listener destroy;
} lock_surface;

struct tektura_lock_manager {
	struct tektura_server *server;
	tektura_lock_state     state;

	struct wlr_session_lock_manager_v1 *wlr_lock_manager;
	struct wlr_session_lock_v1         *active_lock;

	struct wl_list lock_surfaces; /* lista lock_surface */

	/* Listenery */
	struct wl_listener new_lock;
	struct wl_listener lock_manager_destroy;
	struct wl_listener lock_unlock;
	struct wl_listener lock_destroy;
	struct wl_listener new_lock_surface;

	/* Timer timeout dla klienta blokady */
	struct wl_event_source *lock_timer;
};

/* ------------------------------------------------------------------ */
/* Pomocnicze — blokowanie/odblok. wejścia                             */
/* ------------------------------------------------------------------ */

static void inhibit_input(struct tektura_server *server, bool inhibit) {
	/*
	 * Gdy zablokowane: seat nie przekazuje zdarzeń normalnym oknom.
	 * Sterowanie wejściem trafia wyłącznie do powierzchni blokady.
	 * Wystarczy wyczyścić fokus klawiatury i wskaźnika.
	 */
	if (inhibit) {
		wlr_seat_keyboard_clear_focus(server->seat);
		wlr_seat_pointer_clear_focus(server->seat);
		wlr_log(WLR_INFO, "lock: wejście zablokowane");
	} else {
		wlr_log(WLR_INFO, "lock: wejście odblokowane");
	}
}

/* ------------------------------------------------------------------ */
/* Obsługa powierzchni blokady                                          */
/* ------------------------------------------------------------------ */

static void lock_surface_map(struct wl_listener *listener, void *data) {
	lock_surface *ls = wl_container_of(listener, ls, map);
	/*
	 * Po zmapowaniu surface blokady — przekaż fokus klawiatury do niej.
	 * To jest kluczowe: klient blokady musi dostać wejście klawiatury
	 * zanim użytkownik zacznie wpisywać hasło.
	 */
	struct wlr_surface *surface =
		ls->lock_surface->surface;
	struct wlr_keyboard *kb =
		wlr_seat_get_keyboard(ls->mgr->server->seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(ls->mgr->server->seat, surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	}
	ls->mgr->state = LOCK_STATE_LOCKED;
	wlr_log(WLR_INFO, "lock: klient blokady podłączony — ekran zablokowany");
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
	lock_surface *ls = wl_container_of(listener, ls, destroy);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	free(ls);
}

/* ------------------------------------------------------------------ */
/* Zdarzenia protokołu ext-session-lock-v1                             */
/* ------------------------------------------------------------------ */

static void on_new_lock_surface(struct wl_listener *listener, void *data) {
	struct tektura_lock_manager *mgr =
		wl_container_of(listener, mgr, new_lock_surface);
	struct wlr_session_lock_surface_v1 *wlr_surf = data;

	lock_surface *ls = calloc(1, sizeof(*ls));
	ls->mgr = mgr;
	ls->lock_surface = wlr_surf;

	/* Dodaj do drzewa sceny nad wszystkimi okienkamie */
	ls->scene_tree = wlr_scene_subsurface_tree_create(
		&mgr->server->scene->tree, wlr_surf->surface);

	ls->map.notify = lock_surface_map;
	wl_signal_add(&wlr_surf->surface->events.map, &ls->map);
	ls->destroy.notify = lock_surface_destroy;
	wl_signal_add(&wlr_surf->events.destroy, &ls->destroy);

	wl_list_insert(&mgr->lock_surfaces, &ls->link);

	/* Wyślij configure żeby klient wiedział o rozmiarze */
	struct wlr_output *output = wlr_surf->output;
	int w = output->width, h = output->height;
	wlr_session_lock_surface_v1_configure(wlr_surf, w, h);
}

static void on_lock_unlock(struct wl_listener *listener, void *data) {
	struct tektura_lock_manager *mgr =
		wl_container_of(listener, mgr, lock_unlock);

	mgr->state = LOCK_STATE_UNLOCKED;
	mgr->active_lock = NULL;

	inhibit_input(mgr->server, false);

	if (mgr->server->ipc) {
		ipc_broadcast_event(mgr->server->ipc, IPC_EVENT_UNLOCK_SCREEN, NULL);
	}
	wlr_log(WLR_INFO, "lock: ekran odblokowany");
}

static void on_lock_destroy(struct wl_listener *listener, void *data) {
	struct tektura_lock_manager *mgr =
		wl_container_of(listener, mgr, lock_destroy);

	if (mgr->state == LOCK_STATE_LOCKED) {
		/* Klient blokady zgubiony (crash) zanim odblokował — tryb ABANDONED */
		mgr->state = LOCK_STATE_ABANDONED;
		wlr_log(WLR_ERROR,
			"lock: klient blokady zgubiony! Ekran pozostaje zablokowany. "
			"Użyj TTY aby odblokować.");
		/* Weź fokus z powrotem — nikt nic nie dostanie */
		wlr_seat_keyboard_clear_focus(mgr->server->seat);
	}
	mgr->active_lock = NULL;

	wl_list_remove(&mgr->lock_unlock.link);
	wl_list_remove(&mgr->lock_destroy.link);
	wl_list_remove(&mgr->new_lock_surface.link);
}

/* Timer — klient blokady nie podłączył się na czas */
static int on_lock_client_timeout(void *data) {
	struct tektura_lock_manager *mgr = data;
	mgr->lock_timer = NULL;

	if (mgr->state == LOCK_STATE_LOCKING) {
		mgr->state = LOCK_STATE_ABANDONED;
		wlr_log(WLR_ERROR,
			"lock: klient blokady nie podłączył się w ciągu %d ms. "
			"Ekran czarny, wejście zablokowane.",
			LOCK_CLIENT_TIMEOUT_MS);
	}
	return 0;
}

static void on_new_lock(struct wl_listener *listener, void *data) {
	struct tektura_lock_manager *mgr =
		wl_container_of(listener, mgr, new_lock);
	struct wlr_session_lock_v1 *lock = data;

	/* Odrzuć jeśli ekran już zablokowany */
	if (mgr->active_lock != NULL) {
		wlr_session_lock_v1_destroy(lock);
		wlr_log(WLR_DEBUG, "lock: odrzucono duplikat żądania blokady");
		return;
	}

	mgr->active_lock = lock;
	mgr->state = LOCK_STATE_LOCKING;

	/* Zablokuj wejście natychmiast */
	inhibit_input(mgr->server, true);

	/* Ustaw listenery dla tego locka */
	mgr->lock_unlock.notify = on_lock_unlock;
	wl_signal_add(&lock->events.unlock_and_destroy, &mgr->lock_unlock);
	mgr->lock_destroy.notify = on_lock_destroy;
	wl_signal_add(&lock->events.destroy, &mgr->lock_destroy);
	mgr->new_lock_surface.notify = on_lock_surface;
	wl_signal_add(&lock->events.new_surface, &mgr->new_lock_surface);

	/* Poinformuj klientów IPC o blokadzie */
	if (mgr->server->ipc) {
		ipc_broadcast_event(mgr->server->ipc, IPC_EVENT_LOCK_SCREEN, NULL);
	}

	/* Uruchom timer — jeśli klient nie przyjdzie, przechodzimy w ABANDONED */
	struct wl_event_loop *loop =
		wl_display_get_event_loop(mgr->server->wl_display);
	mgr->lock_timer = wl_event_loop_add_timer(loop,
		on_lock_client_timeout, mgr);
	wl_event_source_timer_update(mgr->lock_timer, LOCK_CLIENT_TIMEOUT_MS);

	/* Zatwierdź blokadę po stronie protokołu */
	wlr_session_lock_v1_send_locked(lock);
	wlr_log(WLR_INFO, "lock: zainicjowano blokadę ekranu");
}

static void on_lock_manager_destroy(struct wl_listener *listener, void *data) {
	struct tektura_lock_manager *mgr =
		wl_container_of(listener, mgr, lock_manager_destroy);
	mgr->wlr_lock_manager = NULL;
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

tektura_lock_manager *lock_manager_init(struct tektura_server *server) {
	struct tektura_lock_manager *mgr = calloc(1, sizeof(*mgr));
	if (!mgr) return NULL;
	mgr->server = server;
	mgr->state  = LOCK_STATE_UNLOCKED;
	wl_list_init(&mgr->lock_surfaces);

	mgr->wlr_lock_manager =
		wlr_session_lock_manager_v1_create(server->wl_display);
	if (!mgr->wlr_lock_manager) {
		wlr_log(WLR_ERROR, "lock: błąd inicjalizacji ext-session-lock-v1");
		free(mgr);
		return NULL;
	}

	mgr->new_lock.notify = on_new_lock;
	wl_signal_add(&mgr->wlr_lock_manager->events.new_lock, &mgr->new_lock);

	mgr->lock_manager_destroy.notify = on_lock_manager_destroy;
	wl_signal_add(&mgr->wlr_lock_manager->events.destroy,
		&mgr->lock_manager_destroy);

	wlr_log(WLR_INFO, "lock: menedżer blokady ekranu zainicjalizowany");
	return mgr;
}

void lock_request(tektura_lock_manager *mgr) {
	if (!mgr || mgr->state != LOCK_STATE_UNLOCKED) {
		wlr_log(WLR_DEBUG, "lock: żądanie blokady zignorowane (stan=%d)", mgr ? mgr->state : -1);
		return;
	}
	/*
	 * Wyślij sygnał IPC → karton-lock się podłączy i wywoła
	 * ext_session_lock_v1.lock() przez protokół Wayland.
	 * Właściwa blokada następuje w on_new_lock().
	 */
	wlr_log(WLR_INFO, "lock: żądanie blokady ekranu");
	if (mgr->server->ipc) {
		ipc_broadcast_event(mgr->server->ipc, IPC_EVENT_LOCK_SCREEN, NULL);
	}
}

tektura_lock_state lock_get_state(const tektura_lock_manager *mgr) {
	return mgr ? mgr->state : LOCK_STATE_UNLOCKED;
}

bool lock_is_locked(const tektura_lock_manager *mgr) {
	if (!mgr) return false;
	return mgr->state == LOCK_STATE_LOCKED ||
	       mgr->state == LOCK_STATE_LOCKING ||
	       mgr->state == LOCK_STATE_ABANDONED;
}

void lock_manager_destroy(tektura_lock_manager *mgr) {
	if (!mgr) return;
	if (mgr->lock_timer) {
		wl_event_source_remove(mgr->lock_timer);
	}
	/* Wyczyść listenery jeśli lock aktywny */
	if (mgr->active_lock) {
		wl_list_remove(&mgr->lock_unlock.link);
		wl_list_remove(&mgr->lock_destroy.link);
		wl_list_remove(&mgr->new_lock_surface.link);
	}
	wl_list_remove(&mgr->new_lock.link);
	wl_list_remove(&mgr->lock_manager_destroy.link);
	free(mgr);
}
